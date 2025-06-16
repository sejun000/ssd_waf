#pragma once
#include <vector>
#include <unordered_map>
#include <map>
#include <string>
#include <cstdio>
#include "allocator.h"
#include "icache.h"

// 로그 캐시에서 각 4K 단위의 엔트리를 나타내는 구조체
struct LogEntry {
    long key;   // 논리 블록 주소(LBA)
    bool valid; // 유효한 데이터 여부
};

class LogFIFOCache : public ICache {
public:
    /**
     * 생성자
     * @param capacity: 캐시 블록 단위 최대 개수 (예, 64K 단위로 몇 개)
     * @param _cache_block_size: 캐시 블록 크기 (바이트 단위, 보통 64K 등, 반드시 4K의 배수)
     * @param _cache_trace: 캐시 trace 기록 여부
     * @param trace_file: trace 기록 파일 경로
     */
    LogFIFOCache(uint64_t cold_capacity, long capacity, int _cache_block_size, bool _cache_trace, const std::string &trace_file, const std::string &cold_trace_file);
    virtual ~LogFIFOCache();

    bool exists(long key) override;
    void touch(long key, OP_TYPE op_type) override; // LogFIFOCache에서는 touch는 무시합니다.
    void evict_one_block() override;
    void batch_insert(const std::map<long, int> &newBlocks, OP_TYPE op_type) override;
    bool is_cache_filled() override;
    int get_block_size() override;
    void print_cache_trace(long long lba_offset, int lba_size, OP_TYPE op_type) override;
    size_t size() override;

private:
    long capacity_;         // 캐시 블록 단위의 총 개수
    int cache_block_size;   // 캐시 블록 크기 (바이트 단위)
    bool cache_trace;       // trace 기록 여부
    FILE *cache_trace_fp;   // trace 기록 파일 포인터
    FILE *cold_trace_fp;

    // 내부 로그는 4K 단위로 관리됨.
    std::vector<LogEntry> log_buffer;  // 원형 버퍼 (4K 단위 엔트리)
    size_t write_ptr;                  // 다음 쓰기 위치 (인덱스)

    // 현재 유효한 항목에 대한 매핑: key -> log_buffer 인덱스
    std::unordered_map<long, size_t> mapping;
    size_t old_write_ptr;
    // (필요 시) 외부 할당자 사용 – 생성자에서 capacity * cache_block_size 만큼 할당
    //DummyAllocator allocator;
};
