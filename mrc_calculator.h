#pragma once

#include <vector>
#include <map>
#include <string>

// 알고리즘 선택을 위한 열거형
enum class AlgorithmType {
    LRU,
    OPT
};

class MrcCalculator {
public:
    std::map<long long,double> calculate_mrc(const std::vector<long long>& trace,
                                             long long total_dataset_size,
                                             AlgorithmType type, int num_buckets);

    std::map<long long,double> build_mrc_from_distances(const std::vector<long long>& distances,
                                                        size_t total_accesses,
                                                        long long total_dataset_size,
                                                        int num_buckets);

    // 새로 추가: 구간별
    std::vector<std::pair<size_t, std::map<long long,double>>>
    calculate_mrc_intervals(const std::vector<long long>& trace,
                            long long total_dataset_size,
                            AlgorithmType type, int num_buckets,
                            const std::vector<size_t>& checkpoints);
};
