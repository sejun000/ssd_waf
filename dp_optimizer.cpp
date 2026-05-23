// Dynamic Programming optimizer for U_B trajectory selection from dp.* logs.
//
// 2026-05-21 redesign:
//   - File postfix (dp.0.42 etc.) is no longer treated as the target ratio.
//   - Each row's actual U_B is computed from global_valid_blocks * BLK_SIZE / total_cache_size.
//   - For each step t, all files' samples are collected and sorted by actual_UB.
//   - The optimizer operates on a fine ratio grid (default 0.1%); cost[k][t] is obtained by
//     linear interpolation between adjacent actual_UB samples at step t.
//   - If u_target is outside [min_UB, max_UB] at step t, that (k, t) is a boundary (INF).
//   - Transition: |Δu| per step ≤ HOST_BYTES_PER_STEP / total_cache_size_at_step.
//
// Build: g++ -O2 -std=c++17 -o dp_optimizer dp_optimizer.cpp
// Usage: ./dp_optimizer --dir DIR [--tmax N] [--warmup_tb TB] [--step_skip N]
//                      [--qlc_factor X] [--grid_stride X]

#include <bits/stdc++.h>
#include <filesystem>

using namespace std;
namespace fs = std::filesystem;

static double QLC_FACTOR = 8.64;
static constexpr int MA_WINDOW = 24;
static constexpr int BLK_SIZE = 4096;
static constexpr double HOST_BYTES_PER_STEP = 6.0 * 1024.0 * 1024.0 * 1024.0; // 6 GiB
static constexpr double HOST_BLOCKS_PER_STEP = HOST_BYTES_PER_STEP / BLK_SIZE;

struct Sample {
    double actual_UB;
    double F;
    double u_delta;
    double v_delta;
    long long tc_bytes;
    long long host_bytes; // absolute host write at this step (write_size_to_cache)
    double source_dp;
};

static optional<double> parse_dp_num(const string& name) {
    const string prefix = "dp.";
    if (name.size() < prefix.size()) return nullopt;
    if (!equal(prefix.begin(), prefix.end(), name.begin())) return nullopt;
    string tail = name.substr(prefix.size());
    if (tail.empty()) return nullopt;
    try {
        size_t idx = 0;
        double v = stod(tail, &idx);
        if (idx != tail.size()) return nullopt;
        return v;
    } catch (...) { return nullopt; }
}

static bool parse_line_values(const string& line,
                              long long& compacted,
                              long long& evicted,
                              long long& total_cache_size,
                              long long& write_size_to_cache,
                              long long& global_valid_blocks) {
    compacted = 0; evicted = 0; total_cache_size = 0;
    write_size_to_cache = 0; global_valid_blocks = 0;
    auto pos = line.find("invalidate_blocks:");
    if (pos == string::npos) return false;
    istringstream iss(line.substr(pos));
    string tok;
    if (!(iss >> tok)) return false;
    string num; if (!(iss >> num)) return false;
    while (iss >> tok) {
        string key = tok;
        if (!key.empty() && key.back() == ':') {
            key.pop_back();
            string valstr;
            if (!(iss >> valstr)) break;
            long long val = 0;
            try { val = stoll(valstr); } catch (...) { continue; }
            if      (key == "compacted_blocks")     compacted = val;
            else if (key == "evicted_blocks")       evicted = val;
            else if (key == "total_cache_size")     total_cache_size = val;
            else if (key == "write_size_to_cache")  write_size_to_cache = val;
            else if (key == "global_valid_blocks")  global_valid_blocks = val;
        }
    }
    return true;
}

int main(int argc, char** argv) {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    string dir = ".";
    long long tmax = LLONG_MAX;
    double warmup_tb = 0.0;
    int step_skip = 1;
    double grid_stride = 0.001; // 0.1% ratio grid

    for (int i = 1; i < argc; ++i) {
        string a = argv[i];
        if      (a == "--dir"          && i + 1 < argc) dir = argv[++i];
        else if (a == "--tmax"         && i + 1 < argc) tmax = atoll(argv[++i]);
        else if (a == "--warmup_tb"    && i + 1 < argc) warmup_tb = atof(argv[++i]);
        else if (a == "--step_skip"    && i + 1 < argc) step_skip = atoi(argv[++i]);
        else if (a == "--qlc_factor"   && i + 1 < argc) QLC_FACTOR = atof(argv[++i]);
        else if (a == "--grid_stride"  && i + 1 < argc) grid_stride = atof(argv[++i]);
        else {
            cerr << "Unknown or incomplete argument: " << a << "\n"
                 << "Usage: " << argv[0]
                 << " --dir DIR [--tmax N] [--warmup_tb TB] [--step_skip N]"
                 << " [--qlc_factor X] [--grid_stride X]\n";
            return 2;
        }
    }
    long long warmup_bytes = static_cast<long long>(warmup_tb * 1024LL * 1024 * 1024 * 1024);

    // ---- Phase 1: Read all dp.* files; compute per-row actual_UB and smoothed F ----
    struct FileData {
        double dp_num;
        string filename;
        vector<double> actual_UB;
        vector<double> u_delta;
        vector<double> v_delta;
        vector<double> F;
        vector<long long> tc_bytes;
        vector<long long> host_bytes;       // write_size_to_cache per row (absolute)
        long long warmup_compacted = 0;     // cumulative at first post-warmup row
        long long warmup_evicted = 0;
    };

    auto moving_avg = [](const vector<double>& raw, int win) -> vector<double> {
        size_t n = raw.size();
        vector<double> out(n);
        double sum = 0.0;
        for (size_t i = 0; i < n; ++i) {
            sum += raw[i];
            if (i >= (size_t)win) sum -= raw[i - win];
            size_t cnt = min(i + 1, (size_t)win);
            out[i] = sum / cnt;
        }
        return out;
    };

    vector<FileData> files;
    for (auto& de : fs::directory_iterator(dir)) {
        if (!de.is_regular_file()) continue;
        string name = de.path().filename().string();
        auto dp_opt = parse_dp_num(name);
        if (!dp_opt) continue;

        FileData fd;
        fd.dp_num = *dp_opt;
        fd.filename = de.path().string();

        ifstream ifs(fd.filename);
        if (!ifs) { cerr << "Warning: cannot open " << fd.filename << "\n"; continue; }

        vector<long long> comp, evict, tc, gvb, host_b;
        long long first_comp = -1, first_evict = -1;
        string line;
        int line_skip_cnt = 0;
        while (getline(ifs, line)) {
            if (line.find("invalidate_blocks:") == string::npos) continue;
            long long cval=0, eval=0, tcval=0, wval=0, gval=0;
            if (!parse_line_values(line, cval, eval, tcval, wval, gval)) continue;
            if (warmup_bytes > 0 && wval < warmup_bytes) continue;
            if (first_comp < 0) { first_comp = cval; first_evict = eval; }
            if (step_skip > 1 && (line_skip_cnt++ % step_skip) != 0) continue;
            comp.push_back(cval);
            evict.push_back(eval);
            tc.push_back(tcval);
            gvb.push_back(gval);
            host_b.push_back(wval);
        }
        fd.warmup_compacted = (first_comp  > 0) ? first_comp  : 0;
        fd.warmup_evicted   = (first_evict > 0) ? first_evict : 0;
        fd.host_bytes = move(host_b);
        if (comp.empty()) { cerr << "Warning: no valid lines in " << fd.filename << "\n"; continue; }

        size_t fT = comp.size();
        fd.tc_bytes = move(tc);

        fd.actual_UB.resize(fT, 0.0);
        for (size_t t = 0; t < fT; ++t) {
            if (fd.tc_bytes[t] > 0) {
                double total_blks = (double)fd.tc_bytes[t] / BLK_SIZE;
                fd.actual_UB[t] = (double)gvb[t] / total_blks;
            }
        }

        vector<double> raw_u(fT), raw_v(fT);
        long long up = comp[0], vp = evict[0];
        for (size_t t = 0; t < fT; ++t) {
            raw_u[t] = (t == 0) ? 0.0 : max(0.0, (double)(comp[t] - up));
            raw_v[t] = (t == 0) ? 0.0 : max(0.0, (double)(evict[t] - vp));
            up = comp[t]; vp = evict[t];
        }
        vector<double> sm_u = moving_avg(raw_u, MA_WINDOW);
        vector<double> sm_v = moving_avg(raw_v, MA_WINDOW);

        fd.u_delta = move(raw_u);
        fd.v_delta = move(raw_v);
        fd.F.resize(fT);
        for (size_t t = 0; t < fT; ++t) {
            fd.F[t] = sm_u[t] + QLC_FACTOR * sm_v[t] + HOST_BLOCKS_PER_STEP;
        }
        cerr << "  Loaded " << fd.filename
             << " (dp_num=" << fd.dp_num << ", steps=" << fT << ")\n";
        files.push_back(move(fd));
    }
    if (files.empty()) { cerr << "No dp.* files found in " << dir << "\n"; return 1; }

    // ---- Phase 2: Per-step sample list, sorted by actual_UB ----
    size_t T = 0;
    for (auto& fd : files) T = max(T, fd.F.size());
    if (tmax != LLONG_MAX) T = min(T, static_cast<size_t>(tmax));

    vector<vector<Sample>> samples(T);
    for (auto& fd : files) {
        size_t len = min(T, fd.F.size());
        for (size_t t = 0; t < len; ++t) {
            if (fd.tc_bytes[t] <= 0) continue;
            samples[t].push_back({
                fd.actual_UB[t], fd.F[t],
                fd.u_delta[t], fd.v_delta[t],
                fd.tc_bytes[t],
                (t < fd.host_bytes.size() ? fd.host_bytes[t] : 0LL),
                fd.dp_num
            });
        }
    }
    for (auto& v : samples) {
        sort(v.begin(), v.end(),
             [](const Sample& a, const Sample& b){ return a.actual_UB < b.actual_UB; });
        // dedup near-identical actual_UB (keep higher dp_num)
        if (v.size() > 1) {
            vector<Sample> dedup;
            dedup.reserve(v.size());
            for (auto& s : v) {
                if (!dedup.empty() && fabs(dedup.back().actual_UB - s.actual_UB) < 1e-9) {
                    if (s.source_dp > dedup.back().source_dp) dedup.back() = s;
                } else {
                    dedup.push_back(s);
                }
            }
            v = move(dedup);
        }
    }

    // ---- Phase 3: Fine ratio grid ----
    const double GRID_MIN = 0.00;
    const double GRID_MAX = 0.95;
    const size_t K = static_cast<size_t>(round((GRID_MAX - GRID_MIN) / grid_stride)) + 1;
    vector<double> grid(K);
    for (size_t k = 0; k < K; ++k) grid[k] = GRID_MIN + k * grid_stride;

    // ---- Phase 4: Cost matrix via linear interpolation ----
    const double INF = 1e300;
    vector<vector<double>> cost(K, vector<double>(T, INF));
    vector<vector<double>> cost_u(K, vector<double>(T, 0.0));
    vector<vector<double>> cost_v(K, vector<double>(T, 0.0));
    vector<long long> step_capacity(T, 0);

    for (size_t t = 0; t < T; ++t) {
        auto& list = samples[t];
        if (list.empty()) continue;
        step_capacity[t] = list.front().tc_bytes; // any sample, same trace
        const double lo_ub = list.front().actual_UB;
        const double hi_ub = list.back().actual_UB;
        const double tol = grid_stride * 0.5 + 1e-12;
        for (size_t k = 0; k < K; ++k) {
            double u = grid[k];
            if (u + tol < lo_ub) continue;       // below boundary
            if (u - tol > hi_ub) continue;       // above boundary
            auto it = upper_bound(list.begin(), list.end(), u,
                                  [](double v, const Sample& s){ return v < s.actual_UB; });
            if (it == list.begin()) {
                cost[k][t]   = list.front().F;
                cost_u[k][t] = list.front().u_delta;
                cost_v[k][t] = list.front().v_delta;
            } else if (it == list.end()) {
                cost[k][t]   = list.back().F;
                cost_u[k][t] = list.back().u_delta;
                cost_v[k][t] = list.back().v_delta;
            } else {
                auto hi = it;
                auto lo = prev(it);
                double denom = hi->actual_UB - lo->actual_UB;
                double frac = (denom > 1e-12) ? (u - lo->actual_UB) / denom : 0.0;
                cost[k][t]   = lo->F       + frac * (hi->F       - lo->F);
                cost_u[k][t] = lo->u_delta + frac * (hi->u_delta - lo->u_delta);
                cost_v[k][t] = lo->v_delta + frac * (hi->v_delta - lo->v_delta);
            }
        }
    }

    // ---- Phase 5: Per-step transition bound ----
    vector<int> max_step_bins(T, 0);
    long long min_cap = LLONG_MAX, max_cap = 0;
    for (size_t t = 0; t < T; ++t) {
        long long cap = step_capacity[t];
        if (cap <= 0) continue;
        double max_du = HOST_BYTES_PER_STEP / (double)cap;
        max_step_bins[t] = (int)ceil(max_du / grid_stride - 1e-9);
        min_cap = min(min_cap, cap);
        max_cap = max(max_cap, cap);
    }
    cerr << "T=" << T << ", K=" << K
         << ", grid_stride=" << grid_stride;
    if (max_cap > 0) {
        cerr << ", capacity_bytes=[" << min_cap << "," << max_cap << "]"
             << ", per-step max ±bins ≈ "
             << (int)ceil(HOST_BYTES_PER_STEP / (double)max_cap / grid_stride - 1e-9)
             << ".."
             << (int)ceil(HOST_BYTES_PER_STEP / (double)min_cap / grid_stride - 1e-9);
    }
    cerr << "\n";

    // ---- Phase 6: DP ----
    vector<vector<double>> DP(T, vector<double>(K, INF));
    vector<vector<int>> parent(T, vector<int>(K, -1));
    // Force start at the lowest valid grid at t=0 (lowest reachable U_B boundary).
    int start_k = -1;
    for (size_t k = 0; k < K; ++k) {
        if (cost[k][0] < INF * 0.5) { start_k = (int)k; break; }
    }
    if (start_k >= 0) DP[0][start_k] = cost[start_k][0];
    cerr << "Start anchor: k=" << start_k
         << ", c=" << (start_k >= 0 ? grid[start_k] : -1.0) << "\n";
    for (size_t t = 1; t < T; ++t) {
        int max_d = max_step_bins[t];
        for (size_t k = 0; k < K; ++k) {
            if (cost[k][t] >= INF * 0.5) continue;
            double best = INF; int arg = -1;
            int j_lo = max(0, (int)k - max_d);
            int j_hi = min((int)K - 1, (int)k + max_d);
            for (int j = j_lo; j <= j_hi; ++j) {
                if (DP[t-1][j] >= INF * 0.5) continue;
                double cand = DP[t-1][j] + cost[k][t];
                if (cand < best) { best = cand; arg = j; }
            }
            if (arg >= 0) { DP[t][k] = best; parent[t][k] = arg; }
        }
    }

    // ---- Phase 7: Find best terminal and backtrack ----
    double best = INF; int bestk = -1; size_t last = T - 1;
    for (size_t k = 0; k < K; ++k) {
        if (DP[last][k] < best) { best = DP[last][k]; bestk = (int)k; }
    }
    if (bestk < 0) { cerr << "No feasible DP path found.\n"; return 1; }

    vector<int> choice(T, -1);
    int cur = bestk;
    for (int t = (int)last; t >= 0; --t) {
        choice[t] = cur;
        if (t > 0) cur = parent[t][cur];
    }

    // ---- Phase 8: Output ----
    cout.setf(std::ios::fixed); cout << setprecision(6);
    static constexpr double BLK_TO_TB = 4096.0 / 1e12;
    double total_cost_tb = best * BLK_TO_TB;
    cout << "Steps(T): " << T << ", GridSize(K): " << K
         << ", GridStride: " << grid_stride
         << ", MinTotalCost(blocks): " << best
         << ", TotalCost(TB): " << total_cost_tb << "\n";
    cout << "Best terminal: k=" << bestk << ", c=" << grid[bestk]
         << ", cost=" << best << "\n\n";

    // Warmup pre-cost (cumulative compacted/evicted blocks at first post-warmup row, avg over files)
    double avg_warm_comp = 0.0, avg_warm_evict = 0.0;
    int cnt_warm = 0;
    for (auto& fd : files) {
        if (fd.warmup_compacted > 0 || fd.warmup_evicted > 0) {
            avg_warm_comp  += (double)fd.warmup_compacted;
            avg_warm_evict += (double)fd.warmup_evicted;
            cnt_warm++;
        }
    }
    if (cnt_warm > 0) { avg_warm_comp /= cnt_warm; avg_warm_evict /= cnt_warm; }
    const double TIB = 1024.0 * 1024.0 * 1024.0 * 1024.0;
    const double warmup_host_blocks = warmup_tb * TIB / BLK_SIZE;

    cout << "t, host_TB, chosen_c, base_F_t, cumulative_cost\n";
    double cum = 0.0;
    double total_compaction = 0.0;
    double total_eviction   = 0.0;
    double total_host       = 0.0;
    for (size_t t = 0; t < T; ++t) {
        int k = choice[t];
        double Ft = cost[k][t];
        cum += Ft;
        total_compaction += cost_u[k][t];
        total_eviction   += cost_v[k][t];
        total_host       += HOST_BLOCKS_PER_STEP;
        double host_TB = samples[t].empty()
            ? 0.0
            : (double)samples[t].front().host_bytes / TIB;
        cout << t << ", " << host_TB << ", " << grid[k]
             << ", " << Ft << ", " << cum << "\n";
    }

    cout << "\n=== Cost Breakdown (TB) — DP path only (warmup excluded) ===\n";
    cout << "Host write:  " << total_host * BLK_TO_TB << "\n";
    cout << "Compaction:  " << total_compaction * BLK_TO_TB << "\n";
    cout << "Eviction:    " << total_eviction * BLK_TO_TB
         << " (x" << QLC_FACTOR << " = "
         << total_eviction * QLC_FACTOR * BLK_TO_TB << ")\n";
    cout << "Total:       "
         << (total_host + total_compaction + total_eviction * QLC_FACTOR) * BLK_TO_TB
         << "\n";

    cout << "\n=== Cost Breakdown (TB) — Full simulation (from host write 0) ===\n";
    cout << "Warmup host:     " << warmup_host_blocks * BLK_TO_TB << "\n";
    cout << "Warmup compact:  " << avg_warm_comp * BLK_TO_TB << "\n";
    cout << "Warmup evict:    " << avg_warm_evict * BLK_TO_TB
         << " (x" << QLC_FACTOR << " = " << avg_warm_evict * QLC_FACTOR * BLK_TO_TB << ")\n";
    double full_host    = total_host       + warmup_host_blocks;
    double full_compact = total_compaction + avg_warm_comp;
    double full_evict   = total_eviction   + avg_warm_evict;
    cout << "Host write:  " << full_host    * BLK_TO_TB << "\n";
    cout << "Compaction:  " << full_compact * BLK_TO_TB << "\n";
    cout << "Eviction:    " << full_evict   * BLK_TO_TB
         << " (x" << QLC_FACTOR << " = "
         << full_evict * QLC_FACTOR * BLK_TO_TB << ")\n";
    cout << "Total:       "
         << (full_host + full_compact + full_evict * QLC_FACTOR) * BLK_TO_TB << "\n";
    return 0;
}
