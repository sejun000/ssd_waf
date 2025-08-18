#include "mrc_calculator.h" // mrc_calculator.h에 MrcCalculator 클래스와 AlgorithmType enum이 선언되어 있다고 가정합니다.
#include <iostream>
#include <unordered_map>
#include <algorithm>
#include <limits>
#include <cmath>

// O(log N) 시간 복잡도로 구간 합을 계산하기 위한 Fenwick Tree (BIT)
class FenwickTree {
private:
    // [IMPROVEMENT] 매우 큰 트레이스(> 2^31)에서 발생할 수 있는 정수 오버플로우를 방지하기 위해 long long으로 변경
    std::vector<long long> tree;

public:
    FenwickTree(int size) : tree(size + 1, 0) {}

    void update(int index, int delta) {
        index++; // 1-based index
        while ((size_t)index < tree.size()) {
            tree[index] += delta;
            index += index & -index;
        }
    }

    // [IMPROVEMENT] 반환 타입을 long long으로 변경
    long long query(int index) {
        if (index < 0) {
            return 0; // 0 미만 인덱스 쿼리는 0을 반환
        }
        index++; // 1-based index
        long long sum = 0;
        while (index > 0) {
            sum += tree[index];
            index -= index & -index;
        }
        return sum;
    }
};

// [수정됨] build_mrc_from_distances 함수
// total_dataset_size를 파라미터로 받아서 버킷 크기를 계산합니다.
std::map<long long, double> MrcCalculator::build_mrc_from_distances(const std::vector<long long>& distances, size_t total_accesses, long long total_dataset_size, int num_buckets) {
    if (total_accesses == 0 || total_dataset_size == 0) {
        return {};
    }

    // [변경점!] max_dist 대신 total_dataset_size를 기준으로 bin_size를 계산합니다.
    long long bin_size = std::ceil(static_cast<double>(total_dataset_size) / num_buckets);
    if (bin_size == 0) bin_size = 1;

    std::vector<long long> binned_histogram(num_buckets, 0);
    for (long long dist : distances) {
        if (dist != std::numeric_limits<long long>::max()) {
            // dist가 total_dataset_size를 넘더라도 마지막 버킷에 포함시킵니다.
            int bin_index = (dist - 1) / bin_size;
            if (bin_index >= num_buckets) {
                bin_index = num_buckets - 1;
            }
            if (bin_index >= 0) {
                 binned_histogram[bin_index]++;
            }
        }
    }

    std::map<long long, double> mrc;
    long long cumulative_hits = 0;
    for (int i = 0; i < num_buckets; ++i) {
        cumulative_hits += binned_histogram[i];
        long long current_cache_size = (i + 1) * bin_size;
        
        // 캐시 크기가 데이터셋 크기를 넘지 않도록 조정
        if (current_cache_size > total_dataset_size) {
            current_cache_size = total_dataset_size;
        }

        long long miss_count = total_accesses - cumulative_hits;
        mrc[current_cache_size] = static_cast<double>(miss_count) / total_accesses;
        
        if (current_cache_size == total_dataset_size) break;
    }

    return mrc;
}


// [수정됨] calculate_mrc 함수
// total_dataset_size 파라미터를 추가하여 build_mrc_from_distances로 전달합니다.
std::map<long long, double> MrcCalculator::calculate_mrc(const std::vector<long long>& trace, long long total_dataset_size, AlgorithmType type, int num_buckets) {
    const size_t n = trace.size();
    if (n == 0) {
        return {};
    }

    std::vector<long long> next_or_prev_pos(n);
    std::unordered_map<long long, long long> last_pos;

    if (type == AlgorithmType::LRU) {
        for (size_t i = 0; i < n; ++i) {
            long long block = trace[i];
            if (last_pos.count(block)) {
                next_or_prev_pos[i] = last_pos[block];
            } else {
                next_or_prev_pos[i] = -1;
            }
            last_pos[block] = i;
        }
    } else { // OPT
        for (long long i = n - 1; i >= 0; --i) {
            long long block = trace[i];
            if (last_pos.count(block)) {
                next_or_prev_pos[i] = last_pos[block];
            } else {
                next_or_prev_pos[i] = -1;
            }
            last_pos[block] = i;
        }
    }

    std::vector<long long> distances(n);
    FenwickTree ft(n);
    std::unordered_map<long long, int> last_occurrence;

    if (type == AlgorithmType::LRU) {
        for (size_t i = 0; i < n; ++i) {
            long long prev_pos = next_or_prev_pos[i];
            if (prev_pos == -1) {
                distances[i] = std::numeric_limits<long long>::max();
            } else {
                long long unique_count = ft.query(i - 1) - ft.query(prev_pos);
                distances[i] = unique_count + 1;
            }
            long long current_block = trace[i];
            if (last_occurrence.count(current_block)) {
                ft.update(last_occurrence.at(current_block), -1);
            }
            ft.update(i, 1);
            last_occurrence[current_block] = i;
        }
    } else { // OPT
        for (long long i = n - 1; i >= 0; --i) {
            long long next_pos = next_or_prev_pos[i];
            if (next_pos == -1) {
                distances[i] = std::numeric_limits<long long>::max();
            } else {
                long long unique_count = ft.query(next_pos - 1) - ft.query(i);
                distances[i] = unique_count + 1;
            }
            long long current_block = trace[i];
            if (last_occurrence.count(current_block)) {
                ft.update(last_occurrence.at(current_block), -1);
            }
            ft.update(i, 1);
            last_occurrence[current_block] = i;
        }
    }

    return build_mrc_from_distances(distances, n, total_dataset_size, num_buckets);
}