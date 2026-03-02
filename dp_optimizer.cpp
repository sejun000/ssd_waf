// Dynamic Programming optimizer for valid ratio selection from dp.* logs
//
// Assumptions and rules based on user description:
// - Input files: current directory entries named like "dp.*" where * is the valid ratio number (double), e.g., dp.0.80
// - Each file contains lines, we only parse lines starting with "invalidate_blocks:" followed by key:value pairs.
// - Keys of interest: compacted_blocks, evicted_blocks, total_cache_size
// - These counters are cumulative; we compute deltas per time step t to get u_t (compaction blocks) and v_t (eviction blocks).
// - Base objective per time step: F_t = u_t + 2.37 * v_t
// - Between t->t+1, if we change valid ratio c->c':
//   - If decrease (c' < c): penalty = 2.37 * (c - c') * total_cache_size/4096
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

struct Series {
    string filename;
    double c = 0.0;                          // valid ratio represented by file name
    vector<long long> u_cum;                 // compacted_blocks cumulative
    vector<long long> v_cum;                 // evicted_blocks cumulative
    vector<long long> tc_bytes;              // total_cache_size bytes
    vector<double> u_delta;                  // per-step deltas
    vector<double> v_delta;                  // per-step deltas
    vector<double> F;                        // u + 2.37 * v per-step
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
                              long long& total_cache_size) {
    // Line format example:
    // invalidate_blocks: 342 compacted_blocks: 0 global_valid_blocks: 24436 write_size_to_cache: 100693504 evicted_blocks: 0 write_hit_size: 0 total_cache_size: 2832159886802944 reinsert_blocks: 0
    // We'll scan tokens and read the values we need. Missing keys default to 0.
    compacted = 0; evicted = 0; total_cache_size = 0;
    istringstream iss(line);
    string tok;
    // We only accept lines starting with invalidate_blocks:
    if (!(iss >> tok)) return false;
    if (tok != "invalidate_blocks:") return false;
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
        }
    }
    return true;
}

int main(int argc, char** argv) {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    string dir = ".";
    long long tmax = LLONG_MAX;

    for (int i = 1; i < argc; ++i) {
        string a = argv[i];
        if (a == "--dir" && i + 1 < argc) {
            dir = argv[++i];
        } else if (a == "--tmax" && i + 1 < argc) {
            tmax = atoll(argv[++i]);
        } else {
            cerr << "Unknown or incomplete argument: " << a << "\n";
            cerr << "Usage: " << argv[0] << " [--dir DIR] [--tmax N]\n";
            return 2;
        }
    }

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
        while (getline(ifs, line)) {
            if (!starts_with(line, "invalidate_blocks:")) continue;
            long long cval=0, eval=0, tcval=0;
            if (parse_line_values(line, cval, eval, tcval)) {
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
        // compute deltas and F
        size_t T = s.u_cum.size();
        s.u_delta.resize(T);
        s.v_delta.resize(T);
        s.F.resize(T);
        long long up = 0, vp = 0;
        for (size_t t = 0; t < T; ++t) {
            long long ucur = s.u_cum[t];
            long long vcur = s.v_cum[t];
            long long du = (t == 0) ? ucur : max(0LL, ucur - up);
            long long dv = (t == 0) ? vcur : max(0LL, vcur - vp);
            s.u_delta[t] = static_cast<double>(du);
            s.v_delta[t] = static_cast<double>(dv);
            s.F[t] = s.u_delta[t] + 2.37 * s.v_delta[t];
            up = ucur; vp = vcur;
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
            // increase valid ratio => compaction cost weight 1.0
            return diff * blocks;
        } else if (diff < 0) {
            // decrease valid ratio => eviction cost weight 2.37
            return (-diff) * blocks * 2.37;
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
        for (size_t k = 0; k < K; ++k) {
            double best = INF; int arg = -1;
            if (k == 30) {
                printf("cost k t: %f %ld cache total %.2f\n", cost[k][t], K, tcache_blocks[t]);
            }
            /*
            for (size_t j = 0; j < K; ++j) {
                double cand = DP[t-1][j] + trans_penalty(series[j].c, series[k].c, t) + cost[k][t];
                if (cand < best) { best = cand; arg = static_cast<int>(j); }
            }*/
            for (size_t j = k - 1; j <= k + 1; ++j) {
                if (j < 0 || j >= K) continue;
                double cand = DP[t-1][j] + cost[k][t];
                if (cand < best) { best = cand; arg = static_cast<int>(j); }
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
    cout << "Steps(T): " << T << ", Ratios(K): " << K << ", MinTotalCost: " << best << "\n";
    cout << "Ratios:";
    for (size_t k = 0; k < K; ++k) cout << (k==0?" ":", ") << series[k].c;
    cout << "\n\n";

    cout << "t, chosen_c, base_F_t, trans_penalty, cumulative_cost\n";
    double cum = 0.0;
    for (size_t t = 0; t < T; ++t) {
        int k = choice[t];
        double pen = 0.0;
        if (t > 0) pen = trans_penalty(series[choice[t-1]].c, series[k].c, t);
        double Ft = cost[k][t];
        cum += pen + Ft;
        cout << t << ", " << series[k].c << ", " << Ft << ", " << pen << ", " << cum << "\n";
    }

    return 0;
}
