// Dynamic Programming optimizer for valid ratio selection from dp.* logs
//
// NEW: Uses actual utilization (global_valid_blocks / total_cache_blocks) binned
// at 1% granularity instead of filename-based target ratios.
// If multiple dp files map to the same (bin, step), uses the higher dp filename number.
//
// Build: g++ -O2 -std=c++17 -o dp_optimizer dp_optimizer.cpp
// Usage: ./dp_optimizer [--dir DIR] [--tmax N] [--warmup_tb TB] [--step_skip N] [--decision_interval N]

#include <bits/stdc++.h>
#include <filesystem>

using namespace std;
namespace fs = std::filesystem;

static double QLC_FACTOR = 8.64;

// Series represents a utilization bin (e.g., 0.66)
struct Series {
    string label;
    double c = 0.0;                          // bin ratio (e.g., 0.66)
    vector<long long> tc_bytes;              // total_cache_size bytes per step
    vector<double> u_delta;                  // per-step raw compaction delta
    vector<double> v_delta;                  // per-step raw eviction delta
    vector<double> F;                        // smoothed cost per step
};

static bool starts_with(const string& s, const string& p) {
    return s.size() >= p.size() && equal(p.begin(), p.end(), s.begin());
}

static optional<double> parse_c_from_filename(const string& name) {
    const string prefix = "dp.";
    if (!starts_with(name, prefix)) return nullopt;
    string tail = name.substr(prefix.size());
    if (tail.empty()) return nullopt;
    try {
        size_t idx = 0;
        double v = stod(tail, &idx);
        if (idx != tail.size()) return nullopt;
        return v;
    } catch (...) {
        return nullopt;
    }
}

static bool parse_line_values(const string& line,
                              long long& compacted,
                              long long& evicted,
                              long long& total_cache_size,
                              long long& write_size_to_cache,
                              long long& global_valid_blocks) {
    compacted = 0; evicted = 0; total_cache_size = 0; write_size_to_cache = 0; global_valid_blocks = 0;
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
            if (key == "compacted_blocks") compacted = val;
            else if (key == "evicted_blocks") evicted = val;
            else if (key == "total_cache_size") total_cache_size = val;
            else if (key == "write_size_to_cache") write_size_to_cache = val;
            else if (key == "global_valid_blocks") global_valid_blocks = val;
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
    int decision_interval = 1;

    for (int i = 1; i < argc; ++i) {
        string a = argv[i];
        if (a == "--dir" && i + 1 < argc) {
            dir = argv[++i];
        } else if (a == "--tmax" && i + 1 < argc) {
            tmax = atoll(argv[++i]);
        } else if (a == "--warmup_tb" && i + 1 < argc) {
            warmup_tb = atof(argv[++i]);
        } else if (a == "--step_skip" && i + 1 < argc) {
            step_skip = atoi(argv[++i]);
        } else if (a == "--decision_interval" && i + 1 < argc) {
            decision_interval = atoi(argv[++i]);
        } else if (a == "--qlc_factor" && i + 1 < argc) {
            QLC_FACTOR = atof(argv[++i]);
        } else {
            cerr << "Unknown or incomplete argument: " << a << "\n";
            cerr << "Usage: " << argv[0] << " [--dir DIR] [--tmax N] [--warmup_tb TB] [--step_skip N] [--decision_interval N]\n";
            return 2;
        }
    }
    long long warmup_bytes = static_cast<long long>(warmup_tb * 1024LL * 1024 * 1024 * 1024);

    // ---- Phase 1: Read all dp files, compute per-file deltas ----
    struct FileData {
        double dp_num;
        string filename;
        vector<double> F;
        vector<double> u_delta;
        vector<double> v_delta;
        vector<long long> tc_bytes;
        vector<long long> gvb;
    };

    static constexpr int MA_WINDOW = 24;
    static constexpr double HOST_BLOCKS_PER_STEP = 10.0 * 1024 * 1024 * 1024 / 4096;

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
        auto copt = parse_c_from_filename(name);
        if (!copt) continue;

        FileData fd;
        fd.dp_num = *copt;
        fd.filename = de.path().string();

        ifstream ifs(fd.filename);
        if (!ifs) { cerr << "Warning: cannot open " << fd.filename << "\n"; continue; }

        vector<long long> comp, evict, tc, gvb_vec;
        string line;
        int line_skip_cnt = 0;
        while (getline(ifs, line)) {
            if (line.find("invalidate_blocks:") == string::npos) continue;
            long long cval=0, eval=0, tcval=0, wval=0, gval=0;
            if (parse_line_values(line, cval, eval, tcval, wval, gval)) {
                if (warmup_bytes > 0 && wval < warmup_bytes) continue;
                if (step_skip > 1 && (line_skip_cnt++ % step_skip) != 0) continue;
                comp.push_back(cval);
                evict.push_back(eval);
                tc.push_back(tcval);
                gvb_vec.push_back(gval);
            }
        }
        if (comp.empty()) { cerr << "Warning: no valid lines in " << fd.filename << "\n"; continue; }

        size_t fT = comp.size();
        fd.tc_bytes = move(tc);
        fd.gvb = move(gvb_vec);

        // raw deltas
        vector<double> raw_u(fT), raw_v(fT);
        long long up = comp[0], vp = evict[0];
        for (size_t t = 0; t < fT; ++t) {
            raw_u[t] = (t == 0) ? 0.0 : max(0.0, (double)(comp[t] - up));
            raw_v[t] = (t == 0) ? 0.0 : max(0.0, (double)(evict[t] - vp));
            up = comp[t]; vp = evict[t];
        }

        // moving average for smoothed F
        vector<double> smooth_u = moving_avg(raw_u, MA_WINDOW);
        vector<double> smooth_v = moving_avg(raw_v, MA_WINDOW);

        fd.u_delta.resize(fT);
        fd.v_delta.resize(fT);
        fd.F.resize(fT);
        for (size_t t = 0; t < fT; ++t) {
            fd.u_delta[t] = raw_u[t];
            fd.v_delta[t] = raw_v[t];
            fd.F[t] = smooth_u[t] + QLC_FACTOR * smooth_v[t] + HOST_BLOCKS_PER_STEP;
        }
        cerr << "  Loaded " << fd.filename << " (dp_num=" << fd.dp_num << ", steps=" << fT << ")\n";
        files.push_back(move(fd));
    }

    if (files.empty()) {
        cerr << "No dp.* files found in " << dir << "\n";
        return 1;
    }

    // ---- Phase 2: Build bin grid by actual utilization (1% bins) ----
    size_t T = 0;
    for (auto& fd : files) T = max(T, fd.F.size());
    if (tmax != LLONG_MAX) T = min(T, static_cast<size_t>(tmax));

    struct CellData {
        double F, u_delta, v_delta;
        long long tc_bytes;
        double source_dp;
    };
    // grid[bin_pct][t] — bin_pct is integer (e.g., 66 means 0.66)
    map<int, map<size_t, CellData>> grid;

    for (auto& fd : files) {
        size_t len = min(T, fd.F.size());
        for (size_t t = 0; t < len; ++t) {
            double total_blks = (fd.tc_bytes[t] > 0)
                ? static_cast<double>(fd.tc_bytes[t]) / 4096.0 : 0.0;
            if (total_blks <= 0) continue;
            double actual_util = static_cast<double>(fd.gvb[t]) / total_blks;
            int bin_pct = (int)round(actual_util * 100.0);
            if (bin_pct < 0 || bin_pct > 100) continue;

            auto& cell_map = grid[bin_pct];
            auto it = cell_map.find(t);
            if (it == cell_map.end() || fd.dp_num > it->second.source_dp) {
                cell_map[t] = {fd.F[t], fd.u_delta[t], fd.v_delta[t], fd.tc_bytes[t], fd.dp_num};
            }
        }
    }

    // ---- Phase 3: Convert bins to sorted Series ----
    vector<int> bin_pcts;
    for (auto& [bp, _] : grid) bin_pcts.push_back(bp);
    // map is ordered, so bin_pcts is already sorted ascending

    vector<Series> series;
    for (int bp : bin_pcts) {
        Series s;
        s.c = bp / 100.0;
        s.label = "bin_" + to_string(bp);
        s.F.resize(T, 0.0);
        s.u_delta.resize(T, 0.0);
        s.v_delta.resize(T, 0.0);
        s.tc_bytes.resize(T, 0);

        for (auto& [t, cell] : grid[bp]) {
            if (t < T) {
                s.F[t] = cell.F;
                s.u_delta[t] = cell.u_delta;
                s.v_delta[t] = cell.v_delta;
                s.tc_bytes[t] = cell.tc_bytes;
            }
        }
        series.push_back(move(s));
    }

    cerr << "Bins: " << series.size() << ", T=" << T << "\n";
    for (size_t k = 0; k < series.size(); ++k) {
        size_t filled = grid[bin_pcts[k]].size();
        cerr << "  " << series[k].c << ": " << filled << "/" << T << " steps filled\n";
    }

    // ---- Phase 4: Build cost matrix and tcache_blocks ----
    const size_t K = series.size();
    const double INF = 1e300;

    // tcache_blocks for transition penalty
    vector<double> tcache_blocks(T, 0.0);
    for (size_t t = 0; t < T; ++t) {
        long long bytes = 0;
        for (auto& s : series) {
            if (t < s.tc_bytes.size() && s.tc_bytes[t] > 0) { bytes = s.tc_bytes[t]; break; }
        }
        tcache_blocks[t] = static_cast<double>(bytes) / 4096.0 / 4096.0;
    }

    // cost[k][t] = F if data exists for this (bin, step), else INF
    vector<vector<double>> cost(K, vector<double>(T, INF));
    for (size_t k = 0; k < K; ++k) {
        for (auto& [t, cell] : grid[bin_pcts[k]]) {
            if (t < T) cost[k][t] = series[k].F[t];
        }
    }

    // Report reachability
    for (size_t k = 0; k < K; ++k) {
        size_t reachable = 0;
        for (size_t t = 0; t < T; ++t) if (cost[k][t] < INF * 0.5) reachable++;
        if (reachable < T)
            cerr << "  c=" << series[k].c << ": " << reachable << "/" << T << " steps reachable\n";
    }

    // ---- DP ----
    auto trans_penalty = [&](double c_prev, double c_next, size_t t)->double{
        double diff = c_next - c_prev;
        double blocks = tcache_blocks[t]; // already bytes/4096
        (void)diff; (void)blocks;
        return 0.0;
    };

    vector<vector<double>> DP(T, vector<double>(K, INF));
    vector<vector<int>> parent(T, vector<int>(K, -1));

    // Initialize t=0
    for (size_t k = 0; k < K; ++k) DP[0][k] = cost[k][0];

    // Transitions
    for (size_t t = 1; t < T; ++t) {
        bool can_switch = (t % decision_interval == 0);
        for (size_t k = 0; k < K; ++k) {
            double best = INF; int arg = -1;
            if (can_switch) {
                // allow ±1 normally, ±2 only to skip over INF intermediate
                for (int j = max(0, (int)k - 2); j <= min((int)K - 1, (int)k + 2); ++j) {
                    int dist = abs(j - (int)k);
                    if (dist == 2) {
                        int mid = (j + (int)k) / 2;
                        if (cost[mid][t-1] < INF * 0.5) continue;
                    }
                    double cand = DP[t-1][j] + trans_penalty(series[j].c, series[k].c, t) + cost[k][t];
                    if (cand < best) { best = cand; arg = j; }
                }
            } else {
                best = DP[t-1][k] + cost[k][t];
                arg = (int)k;
            }
            DP[t][k] = best; parent[t][k] = arg;
        }
    }

    // Recover best final state
    double best = INF; int bestk = -1; size_t last = T - 1;
    cout << "\nFinal DP values at t=" << last << ":\n";
    for (size_t k = 0; k < K; ++k) {
        cout << "  c=" << series[k].c << ": " << DP[last][k] << "\n";
        if (DP[last][k] < best) { best = DP[last][k]; bestk = static_cast<int>(k); }
    }
    cout << "Best final state: k=" << bestk << ", c=" << series[bestk].c << ", cost=" << best << "\n";

    // Backtrack
    vector<int> choice(T, -1);
    int cur = bestk;
    for (int t = static_cast<int>(last); t >= 0; --t) {
        choice[t] = cur;
        if (t > 0) cur = parent[t][cur];
    }

    // Print results
    cout.setf(std::ios::fixed); cout << setprecision(6);
    static constexpr double BLK_TO_TB = 4096.0 / 1e12; // TB (10^12)
    double total_cost_tb = best * BLK_TO_TB;
    cout << "Steps(T): " << T << ", Ratios(K): " << K << ", MinTotalCost(blocks): " << best
         << ", TotalCost(TB): " << total_cost_tb << "\n";
    cout << "Ratios:";
    for (size_t k = 0; k < K; ++k) cout << (k==0?" ":", ") << series[k].c;
    cout << "\n\n";

    cout << "t, chosen_c, base_F_t, trans_penalty, cumulative_cost\n";
    double cum = 0.0;
    double total_compaction = 0.0;
    double total_eviction = 0.0;
    double total_host = 0.0;
    for (size_t t = 0; t < T; ++t) {
        int k = choice[t];
        double pen = 0.0;
        if (t > 0) pen = trans_penalty(series[choice[t-1]].c, series[k].c, t);
        double Ft = cost[k][t];
        cum += pen + Ft;

        // breakdown
        total_compaction += series[k].u_delta[t];
        total_eviction += series[k].v_delta[t];
        total_host += HOST_BLOCKS_PER_STEP;
        if (t > 0) {
            double diff = series[k].c - series[choice[t-1]].c;
            if (diff > 0) {
                total_compaction += pen;
            } else if (diff < 0) {
                total_eviction += pen / QLC_FACTOR;
            }
        }

        cout << t << ", " << series[k].c << ", " << Ft << ", " << pen << ", " << cum << "\n";
    }

    cout << "\n=== Cost Breakdown (TB) ===\n";
    cout << "Host write:  " << total_host * BLK_TO_TB << "\n";
    cout << "Compaction:  " << total_compaction * BLK_TO_TB << "\n";
    cout << "Eviction:    " << total_eviction * BLK_TO_TB << " (x" << QLC_FACTOR << " = " << total_eviction * QLC_FACTOR * BLK_TO_TB << ")\n";
    cout << "Total:       " << (total_host + total_compaction + total_eviction * QLC_FACTOR) * BLK_TO_TB << "\n";

    return 0;
}
