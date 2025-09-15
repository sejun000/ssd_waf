#include "mrc_calculator.h"
#include <unordered_map>
#include <vector>
#include <map>
#include <limits>
#include <cmath>
#include <cstddef>
#include <sys/mman.h>

// ===== 코어덤프 크기 절약 유틸 =====
template <typename T>
static inline void dontdump_vec(std::vector<T>& v) {
#if defined(__linux__) && defined(MADV_DONTDUMP)
    if (!v.empty()) {
        (void)madvise((void*)v.data(), v.size()*sizeof(T), MADV_DONTDUMP);
    }
#endif
}

// ===== Fenwick Tree (size_t 인덱스) =====
class FenwickTree {
    std::vector<long long> tree;
    static inline size_t lowbit(size_t x) { return x & (~x + 1); }
public:
    explicit FenwickTree(size_t n) : tree(n + 1, 0) {}
    void update(size_t i, long long delta) {
        ++i;
        while (i < tree.size()) { tree[i] += delta; i += lowbit(i); }
    }
    long long query(size_t i) {
        long long s = 0;
        ++i;
        while (i > 0) { s += tree[i]; i -= lowbit(i); }
        return s;
    }
    void mark_dontdump() { dontdump_vec(tree); }
};

// ===== 내부 유틸: distances 계산 (LRU/OPT) =====
static void compute_distances(const std::vector<long long>& trace,
                              AlgorithmType type,
                              std::vector<long long>& distances)
{
    const size_t n = trace.size();
    distances.assign(n, 0);
    dontdump_vec(distances);

    // 이전/다음 위치
    std::vector<long long> link(n, -1);
    dontdump_vec(link);

    std::unordered_map<long long, size_t> last;
    last.reserve(n / 8 + 1);

    if (type == AlgorithmType::LRU) {
        for (size_t i = 0; i < n; ++i) {
            auto it = last.find(trace[i]);
            link[i] = (it == last.end() ? -1 : static_cast<long long>(it->second));
            last[trace[i]] = i;
        }
    } else { // OPT
        for (size_t i = n; i-- > 0; ) {
            auto it = last.find(trace[i]);
            link[i] = (it == last.end() ? -1 : static_cast<long long>(it->second));
            last[trace[i]] = i;
        }
    }

    FenwickTree ft(n);
    ft.mark_dontdump();
    std::unordered_map<long long, size_t> occ;
    occ.reserve(n / 8 + 1);

    if (type == AlgorithmType::LRU) {
        for (size_t i = 0; i < n; ++i) {
            long long prev = link[i];
            if (prev == -1) {
                distances[i] = std::numeric_limits<long long>::max();
            } else {
                size_t p = static_cast<size_t>(prev);
                long long unique_count = ft.query(i - 1) - ft.query(p);
                distances[i] = unique_count + 1;
            }
            auto it = occ.find(trace[i]);
            if (it != occ.end()) ft.update(it->second, -1);
            ft.update(i, +1);
            occ[trace[i]] = i;
        }
    } else { // OPT
        for (size_t i = n; i-- > 0; ) {
            long long nextp = link[i];
            if (nextp == -1) {
                distances[i] = std::numeric_limits<long long>::max();
            } else {
                size_t np = static_cast<size_t>(nextp);
                long long unique_count = ft.query(np - 1) - ft.query(i);
                distances[i] = unique_count + 1;
            }
            auto it = occ.find(trace[i]);
            if (it != occ.end()) ft.update(it->second, -1);
            ft.update(i, +1);
            occ[trace[i]] = i;
        }
    }
}

// ===== 기존 단일 MRC =====
std::map<long long, double>
MrcCalculator::build_mrc_from_distances(const std::vector<long long>& distances,
                                        size_t total_accesses,
                                        long long total_dataset_size,
                                        int num_buckets)
{
    if (total_accesses == 0 || total_dataset_size <= 0 || num_buckets <= 0) return {};

    long long bin_size = std::llround(std::ceil((double)total_dataset_size / num_buckets));
    if (bin_size <= 0) bin_size = 1;

    std::vector<long long> bins(num_buckets, 0);
    for (size_t i = 0; i < total_accesses; ++i) {
        long long d = distances[i];
        if (d == std::numeric_limits<long long>::max()) continue;
        long long idx = (d - 1) / bin_size;
        if (idx < 0) idx = 0;
        if (idx >= num_buckets) idx = num_buckets - 1;
        ++bins[(size_t)idx];
    }

    std::map<long long, double> mrc;
    long long hit_cum = 0;
    for (int b = 0; b < num_buckets; ++b) {
        hit_cum += bins[b];
        long long cache_blocks = (long long)(b + 1) * bin_size;
        if (cache_blocks > total_dataset_size) cache_blocks = total_dataset_size;
        long long miss_cnt = (long long)total_accesses - hit_cum;
        mrc[cache_blocks] = (double)miss_cnt / (double)total_accesses;
        if (cache_blocks == total_dataset_size) break;
    }
    return mrc;
}

std::map<long long, double>
MrcCalculator::calculate_mrc(const std::vector<long long>& trace,
                             long long total_dataset_size,
                             AlgorithmType type,
                             int num_buckets)
{
    std::vector<long long> distances;
    compute_distances(trace, type, distances);
    return build_mrc_from_distances(distances, trace.size(), total_dataset_size, num_buckets);
}

// ===== 새로 추가: 구간별 MRC =====
std::vector<std::pair<size_t, std::map<long long,double>>>
MrcCalculator::calculate_mrc_intervals(const std::vector<long long>& trace,
                                       long long total_dataset_size,
                                       AlgorithmType type,
                                       int num_buckets,
                                       const std::vector<size_t>& checkpoints)
{
    const size_t n = trace.size();
    std::vector<std::pair<size_t, std::map<long long,double>>> out;
    if (n == 0 || checkpoints.empty()) return out;

    // 1) 거리 계산 1회
    std::vector<long long> distances;
    compute_distances(trace, type, distances);

    // 2) 히스토그램 누적하며 체크포인트마다 MRC 산출
    if (total_dataset_size <= 0) return out;
    long long bin_size = std::llround(std::ceil((double)total_dataset_size / num_buckets));
    if (bin_size <= 0) bin_size = 1;

    std::vector<long long> bins(num_buckets, 0);
    size_t cp_idx = 0;

    for (size_t i = 0; i < n; ++i) {
        long long d = distances[i];
        if (d != std::numeric_limits<long long>::max()) {
            long long idx = (d - 1) / bin_size;
            if (idx < 0) idx = 0;
            if (idx >= num_buckets) idx = num_buckets - 1;
            ++bins[(size_t)idx];
        }

        // i+1 (처리된 접근 수)가 체크포인트에 도달하면 MRC 한 번 출력
        while (cp_idx < checkpoints.size() && (i + 1) == checkpoints[cp_idx]) {
            const size_t total_accesses = checkpoints[cp_idx];

            // 누적 hits → 미스율로 변환
            std::map<long long,double> mrc;
            long long hit_cum = 0;
            for (int b = 0; b < num_buckets; ++b) {
                hit_cum += bins[b];
                long long cache_blocks = (long long)(b + 1) * bin_size;
                if (cache_blocks > total_dataset_size) cache_blocks = total_dataset_size;
                long long miss_cnt = (long long)total_accesses - hit_cum;
                mrc[cache_blocks] = (double)miss_cnt / (double)total_accesses;
                if (cache_blocks == total_dataset_size) break;
            }

            out.emplace_back(total_accesses, std::move(mrc));
            ++cp_idx;
        }
        if (cp_idx >= checkpoints.size()) break;
    }

    // 혹시 마지막 체크포인트가 n보다 크거나 같도록 들어왔을 때도 안전
    for (; cp_idx < checkpoints.size(); ++cp_idx) {
        size_t total_accesses = std::min(checkpoints[cp_idx], n);
        std::map<long long,double> mrc;
        long long hit_cum = 0;
        for (int b = 0; b < num_buckets; ++b) {
            hit_cum += bins[b];
            long long cache_blocks = (long long)(b + 1) * bin_size;
            if (cache_blocks > total_dataset_size) cache_blocks = total_dataset_size;
            long long miss_cnt = (long long)total_accesses - hit_cum;
            mrc[cache_blocks] = (double)miss_cnt / (double)total_accesses;
            if (cache_blocks == total_dataset_size) break;
        }
        out.emplace_back(total_accesses, std::move(mrc));
    }
    return out;
}
