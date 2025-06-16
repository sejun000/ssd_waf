/**
 * trace_utilization_bitmap.cpp
 *
 * 빌드:
 *   g++ -O3 -std=c++17 -o trace_utilization_bitmap trace_utilization_bitmap.cpp
 *
 * 사용:
 *   ./trace_utilization_bitmap <trace.csv> <device_size_in_bytes>
 *
 * 비트맵 사양
 *   ─ 1bit  = 512 bytes       (섹터 크기)
 *   ─ 1u64  = 64bits = 32 KiB
 *   ─ 메모리 = ceil(dev_size / 512 / 8) 바이트
 *              예) 1.8 TB → ≈ 450 MB
 */
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "trace_parser.h"   // 기존 CSV 파서 그대로 사용

static inline void set_sector_range(uint64_t first, uint64_t last,
                                    std::vector<uint64_t>& bm)
{
    // [first, last) : 섹터 인덱스 (512 B 단위)
    uint64_t word_beg = first >> 6;
    uint64_t word_end = (last - 1) >> 6;

    const uint64_t head_mask = ~0ULL << (first & 63);
    const uint64_t tail_mask = ~0ULL >> (63 - ((last - 1) & 63));

    if (word_beg == word_end) {          // 같은 u64 안에 있음
        bm[word_beg] |= (head_mask & tail_mask);
        return;
    }

    bm[word_beg] |= head_mask;
    bm[word_end] |= tail_mask;
    for (uint64_t w = word_beg + 1; w < word_end; ++w)
        bm[w] = ~0ULL;
}

int main(int argc, char* argv[])
{
    if (argc != 3) {
        std::cerr << "usage: " << argv[0]
                  << " <trace.csv> <device_size_bytes>\n";
        return 1;
    }

    const char* trace_path   = argv[1];
    uint64_t    dev_size     = std::strtoull(argv[2], nullptr, 10);
    const uint64_t SECTOR    = 512;

    /* ────────── 비트맵 확보 ────────── */
    uint64_t num_sectors  = (dev_size + SECTOR - 1) / SECTOR;
    uint64_t num_u64      = (num_sectors + 63) / 64;
    std::vector<uint64_t> bitmap(num_u64, 0);

    /* ────────── 트레이스 파싱 ────────── */
    std::ifstream in(trace_path);
    if (!in) { perror("open trace"); return 1; }

    ITraceParser* parser = createTraceParser("csv");
    std::string line;
    while (std::getline(in, line)) {
        ParsedRow row = parser->parseTrace(line);
        if (row.op_type != "W" && row.op_type != "w") continue;
        if (row.lba_size == 0)                         continue;

        uint64_t first = row.lba_offset / SECTOR;
        uint64_t last  = (row.lba_offset + row.lba_size + SECTOR - 1) / SECTOR;
        set_sector_range(first, last, bitmap);
    }
    in.close();

    /* ────────── set-bit 개수 집계 ────────── */
    uint64_t touched_sectors = 0;
    for (uint64_t w : bitmap)
        touched_sectors += __builtin_popcountll(w);

    uint64_t utilized_bytes = touched_sectors * SECTOR;
    double utilization_pct  =
        100.0 * static_cast<double>(utilized_bytes) /
        static_cast<double>(dev_size);

    std::cout << "Utilized bytes: " << utilized_bytes   << '\n'
              << "Device size   : " << dev_size         << '\n'
              << "Utilization   : " << utilization_pct  << " %\n";
    return 0;
}