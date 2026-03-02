// Dynamic Programming optimizer for valid ratio selection from dp.* logs
//
// Assumptions and rules based on user description:
// - Input files: current directory entries named like "dp.*" where * is the valid ratio number (double), e.g., dp.0.80
// - Each file contains lines, we only parse lines starting with "invalidate_blocks:" followed by key:value pairs.
// - Keys of interest: compacted_blocks, evicted_blocks, total_cache_size
// - These counters are cumulative; we compute deltas per time step t to get u_t (compaction blocks) and v_t (eviction blocks).
// - Base objective per time step: F_t = u_t + QLC_FACTOR * v_t
// - Between t->t+1, if we change valid ratio c->c':
//   - If decrease (c' < c): penalty = QLC_FACTOR * (c - c') * total_cache_size/4096
//   - If increase (c' > c): penalty = 1.0  * (c' - c) * total_cache_size/4096
//   - If unchanged: no penalty
// - total_cache_size in logs is bytes but needs to be divided by 4096 to convert to blocks (as per user note).
// - We assume no initial penalty at t=0.
// - If files have different lengths, we truncate to the minimum available length across all files.
//
// Output:
// - Prints summary with T (steps), K (ratios), minimal total cost.
// - Then prints per-time-step: t, chosen ratio, base F_t, transition penalty from previous ratio, and cumulative cost.
//
// Build: g++ -O2 -std=c++17 -o dp_optimizer dp_optimizer.cpp
// Usage: ./dp_optimizer [--dir DIR] [--tmax N]
//   --dir: directory to scan for dp.* files (default: current directory)
//   --tmax: cap number of time steps used (optional)

#include <bits/stdc++.h>
#include <filesystem>

using namespace std;
namespace fs = std::filesystem;

static constexpr double QLC_FACTOR = 8.64;

struct Series {
    string filename;
    double c = 0.0;                          // valid ratio represented by file name
    vector<long long> u_cum;                 // compacted_blocks cumulative
    vector<long long> v_cum;                 // evicted_blocks cumulative
    vector<long long> tc_bytes;              // total_cache_size bytes
    vector<double> u_delta;                  // per-step deltas
    vector<double> v_delta;                  // per-step deltas
    vector<double> F;                        // u + QLC_FACTOR * v per-step
};

static bool starts_with(const string& s, const string& p) {
    return s.size() >= p.size() && equal(p.begin(), p.end(), s.begin());
}

static optional<double> parse_c_from_filename(const string& name) {
    // Expect pattern: dp.<number>
    const string prefix = "dp.";
    if (!starts_with(name, prefix)) return nullopt;
    string tail = name.substr(prefix.size());
    // Ensure tail is a valid number (allow digits, dot, minus)
    if (tail.empty()) return nullopt;
    try {
        size_t idx = 0;
        double v = stod(tail, &idx);
        if (idx != tail.size()) return nullopt; // extra junk
        return v;
    } catch (...) {
        return nullopt;
    }
}

static bool parse_line_values(const string& line,
                              long long& compacted,
                              long long& evicted,
                              long long& total_cache_size,
                              long long& write_size_to_cache) {
    // Line format example:
    // invalidate_blocks: 342 compacted_blocks: 0 global_valid_blocks: 24436 write_size_to_cache: 100693504 evicted_blocks: 0 write_hit_size: 0 total_cache_size: 2832159886802944 reinsert_blocks: 0
    // We'll scan tokens and read the values we need. Missing keys default to 0.
    compacted = 0; evicted = 0; total_cache_size = 0; write_size_to_cache = 0;
    // Find "invalidate_blocks:" anywhere in the line (may have prefix like "LOG_GREEDY_11")
    auto pos = line.find("invalidate_blocks:");
    if (pos == string::npos) return false;
    istringstream iss(line.substr(pos));
    string tok;
    if (!(iss >> tok)) return false; // "invalidate_blocks:"
    // skip the number after invalidate_blocks:
    string num; if (!(iss >> num)) return false;
    // Read remaining key: value pairs
    while (iss >> tok) {
        string key = tok; // should have trailing ':'
        if (!key.empty() && key.back() == ':') {
            key.pop_back();
            string valstr;
            if (!(iss >> valstr)) break;
            // convert to long long if numeric
            long long val = 0;
            try {
                val = stoll(valstr);
            } catch (...) {
                continue;
            }
            if (key == "compacted_blocks") compacted = val;
            else if (key == "evicted_blocks") evicted = val;
            else if (key == "total_cache_size") total_cache_size = val;
            else if (key == "write_size_to_cache") write_size_to_cache = val;
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
        } else {
            cerr << "Unknown or incomplete argument: " << a << "\n";
            cerr << "Usage: " << argv[0] << " [--dir DIR] [--tmax N] [--warmup_tb TB] [--step_skip N] [--decision_interval N]\n";
            return 2;
        }
    }
    long long warmup_bytes = static_cast<long long>(warmup_tb * 1024LL * 1024 * 1024 * 1024);

    vector<Series> series;
    // Scan directory for dp.* files
    for (auto& de : fs::directory_iterator(dir)) {
        if (!de.is_regular_file()) continue;
        string name = de.path().filename().string();
        auto copt = parse_c_from_filename(name);
        if (!copt) continue;
        Series s;
        s.filename = de.path().string();
        s.c = *copt;
        ifstream ifs(s.filename);
        if (!ifs) {
            cerr << "Warning: cannot open " << s.filename << "\n";
            continue;
        }
        string line;
        vector<long long> comp, evict, tc;
        comp.reserve(100000);
        evict.reserve(100000);
        tc.reserve(100000);
        int line_skip_cnt = 0;
        while (getline(ifs, line)) {
            if (line.find("invalidate_blocks:") == string::npos) continue;
            long long cval=0, eval=0, tcval=0, wval=0;
            if (parse_line_values(line, cval, eval, tcval, wval)) {
                if (warmup_bytes > 0 && wval < warmup_bytes) continue;
                if (step_skip > 1 && (line_skip_cnt++ % step_skip) != 0) continue;
                comp.push_back(cval);
                evict.push_back(eval);
                tc.push_back(tcval);
            }
        }
        if (comp.empty()) {
            cerr << "Warning: no valid lines in " << s.filename << "\n";
            continue;
        }
        s.u_cum = move(comp);
        s.v_cum = move(evict);
        s.tc_bytes = move(tc);
        // compute raw deltas, then apply moving average
        size_t T = s.u_cum.size();
        s.u_delta.resize(T);
        s.v_delta.resize(T);
        s.F.resize(T);
        static constexpr double HOST_BLOCKS_PER_STEP = 10.0 * 1024 * 1024 * 1024 / 4096;
        static constexpr int MA_WINDOW = 120;

        // raw deltas
        vector<double> raw_u(T), raw_v(T);
        long long up = s.u_cum[0], vp = s.v_cum[0];
        for (size_t t = 0; t < T; ++t) {
            raw_u[t] = (t == 0) ? 0.0 : max(0.0, (double)(s.u_cum[t] - up));
            raw_v[t] = (t == 0) ? 0.0 : max(0.0, (double)(s.v_cum[t] - vp));
            up = s.u_cum[t]; vp = s.v_cum[t];
        }

        // moving average
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

        vector<double> smooth_u = moving_avg(raw_u, MA_WINDOW);
        vector<double> smooth_v = moving_avg(raw_v, MA_WINDOW);

        for (size_t t = 0; t < T; ++t) {
            s.u_delta[t] = smooth_u[t];
            s.v_delta[t] = smooth_v[t];
            s.F[t] = smooth_u[t] + QLC_FACTOR * smooth_v[t] + HOST_BLOCKS_PER_STEP;
        }
        series.push_back(move(s));
    }

    if (series.empty()) {
        cerr << "No dp.* files found in " << dir << "\n";
        return 1;
    }

    // Sort series by c for readability
    sort(series.begin(), series.end(), [](const Series& a, const Series& b){
        if (a.c == b.c) return a.filename < b.filename;
        return a.c < b.c;
    });

    // Determine T = min length across all series
    size_t T = series.front().F.size();
    for (auto& s : series) T = min(T, s.F.size());
    if (tmax != LLONG_MAX) T = min(T, static_cast<size_t>(tmax));

    // Build a reference total_cache_blocks per t. Prefer first series.
    vector<double> tcache_blocks(T, 0.0);
    for (size_t t = 0; t < T; ++t) {
        long long bytes = 0;
        // If any series has tc_bytes[t] > 0, use it
        for (auto& s : series) {
            if (t < s.tc_bytes.size() && s.tc_bytes[t] > 0) { bytes = s.tc_bytes[t]; break; }
        }
        // As per instruction: divide by 4096 twice (total_cache_size is wrong by 4096x)
        tcache_blocks[t] = static_cast<double>(bytes) / 4096.0 / 4096.0;
    }

    const size_t K = series.size();
    // Prepare cost matrix cost[k][t] = F_t for choosing c_k at t
    vector<vector<double>> cost(K, vector<double>(T, 0.0));
    for (size_t k = 0; k < K; ++k) {
        for (size_t t = 0; t < T; ++t) {
            cost[k][t] = series[k].F[t];
        }
    }

    auto trans_penalty = [&](double c_prev, double c_next, size_t t)->double{
        double diff = c_next - c_prev;
        double blocks = tcache_blocks[t]; // already bytes/4096
        if (diff > 0) {
            // increase valid ratio => need more valid blocks via TLC writes (weight 1.0)
            return diff * blocks;
        } else if (diff < 0) {
            // decrease valid ratio => eviction cost weight QLC_FACTOR
            return (-diff) * blocks * QLC_FACTOR;
        } else {
            return 0.0;
        }
    };

    // DP arrays
    const double INF = 1e300;
    vector<vector<double>> DP(T, vector<double>(K, INF));
    vector<vector<int>> parent(T, vector<int>(K, -1));

    // Initialize t=0 (no transition penalty at start)
    for (size_t k = 0; k < K; ++k) DP[0][k] = cost[k][0];

    // Transitions
    for (size_t t = 1; t < T; ++t) {
        bool can_switch = (t % decision_interval == 0);
        for (size_t k = 0; k < K; ++k) {
            double best = INF; int arg = -1;
            if (can_switch) {
                // allow k-1, k, k+1
                for (int j = max(0, (int)k - 1); j <= min((int)K - 1, (int)k + 1); ++j) {
                    double cand = DP[t-1][j] + trans_penalty(series[j].c, series[k].c, t) + cost[k][t];
                    if (cand < best) { best = cand; arg = j; }
                }
            } else {
                // must stay at same ratio
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

    // Backtrack path of chosen indices
    vector<int> choice(T, -1);
    int cur = bestk;
    for (int t = static_cast<int>(last); t >= 0; --t) {
        choice[t] = cur;
        if (t > 0) cur = parent[t][cur];
    }

    // Print results
    cout.setf(std::ios::fixed); cout << setprecision(6);
    static constexpr double BLK_TO_TB = 4096.0 / (1024.0*1024*1024*1024);
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
    static constexpr double HOST_BLOCKS_PER_STEP = 10.0 * 1024 * 1024 * 1024 / 4096;
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
                total_compaction += pen; // increase → compaction overhead
            } else if (diff < 0) {
                total_eviction += pen / QLC_FACTOR; // decrease → evict blocks (pen already has QLC_FACTOR)
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
