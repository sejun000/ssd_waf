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
    /**
     * @brief 주어진 트레이스에 대해 MRC(Miss Rate Curve)를 계산합니다.
     * @param trace 분석할 블록 접근 트레이스.
     * @param type 사용할 알고리즘 (LRU 또는 OPT).
     * @return 캐시 크기에 따른 Miss Rate를 담은 맵.
     */
    std::map<long long, double> calculate_mrc(const std::vector<long long>& trace, long long total_dataset_size, AlgorithmType type, int num_buckets);
private:
    /**
     * @brief 거리 히스토그램으로부터 MRC를 생성합니다.
     * @param distances 모든 접근에 대한 (스택/OPT) 거리 벡터.
     * @param total_accesses 전체 접근 횟수.
     * @return 캐시 크기별 Miss Rate 맵.
     */
    std::map<long long, double> build_mrc_from_distances(const std::vector<long long>& distances, size_t total_accesses, long long total_dataset_size, int num_buckets); 
};