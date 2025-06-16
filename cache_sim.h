#ifndef CACHE_SIM_H
#define CACHE_SIM_H

#include <string>
#include <list>
#include <tuple> 
#include "allocator.h"
#include "lru_cache.h"
#include "icache.h"

// 파싱된 트레이스 한 줄의 정보를 저장할 구조체
struct ParsedRow {
    std::string dev_id;
    std::string op_type;
    long long lba_offset;
    int lba_size;
    double timestamp;
};


// 캐시 hit 계산: 주어진 lba 범위에 대해 캐시와 겹치는 바이트 수(hit_bytes)를 반환하고, hit된 블록은 최신으로 갱신
//std::tuple<long long, int, int, int, int> check_cache_hit(ICache &cache, long long lba_offset, int lba_size, int block_size, OP_TYPE op_type);

// LRU 캐시 정책: 주어진 lba 범위의 블록들을 캐시에 추가 (capacity 초과 시 LRU 순서대로 제거)
int lru_cache_policy(ICache &cache, long long lba_offset, int lba_size, long cache_size, int block_size, int hit_blocks, OP_TYPE op_type);

// Read/Write hit ratio를 계산 (퍼센트)
void calc_hit_ratio(long long read_hit_size, long long total_read_size,
                    long long write_hit_size, long long total_write_size,
                    double &read_hit_ratio, double &write_hit_ratio);

#endif // CACHE_SIM_H