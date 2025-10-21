// count_64k_writes.cpp
// 빌드: g++ -O2 -std=c++17 -o count_64k_writes count_64k_writes.cpp
// 사용: ./count_64k_writes <trace.csv> <parser_type> [target_bytes]
// 예시: ./count_64k_writes alibaba.csv alibaba 6000000000000

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <fstream>
#include <string>
#include <algorithm>
#include <cassert>
#include "trace_parser.h"

using u64 = uint64_t;

static constexpr u64 ALIGN = 64ull * 1024;                   // 64 KiB
static constexpr u64 DEFAULT_TARGET = 6ull * 1000 * 1000 * 1000 * 1000; // 6 TB (decimal)

static inline u64 count_64k_blocks(u64 offset, u64 size) {
    // size > 0 가정. [offset, offset+size) 이 겹치는 64KiB 블록 개수
    u64 start_blk = offset / ALIGN;
    u64 end_blk   = (offset + size - 1) / ALIGN;
    return end_blk - start_blk + 1;
}

int main(int argc, char* argv[]) {
    if (argc < 3 || argc > 4) {
        std::cerr << "usage: " << argv[0] << " <trace.csv> <parser_type> [target_bytes]\n";
        std::cerr << "default target_bytes = 6000000000000 (6 TB)\n";
        return 1;
    }

    const char* path = argv[1];
    const std::string parser_type = argv[2];
    u64 target_bytes = (argc == 4) ? std::strtoull(argv[3], nullptr, 10) : DEFAULT_TARGET;

    std::ifstream in(path);
    if (!in) {
        perror("open trace");
        return 1;
    }

    std::unique_ptr<ITraceParser> parser(createTraceParser(parser_type));
    if (!parser) {
        std::cerr << "unknown parser_type: " << parser_type << "\n";
        return 2;
    }

    u64 remaining = target_bytes;
    u64 total_events = 0;
    u64 total_64k_writes = 0;
    u64 bytes_accounted = 0;

    std::string line;
    while (remaining > 0 && std::getline(in, line)) {
        ParsedRow row = parser->parseTrace(line);
        if (!(row.op_type == "W" || row.op_type == "w")) continue;

        u64 off  = row.lba_offset; // 바이트 단위여야 함 (섹터라면 파서에서 512x 변환 필요)
        u64 size = row.lba_size;

        if (size == 0) continue;

        // 타깃(remaining)을 넘지 않게 현재 이벤트를 잘라서 반영
        u64 apply = (size > remaining) ? remaining : size;

        // 64KiB 블록 개수 계산
        u64 blocks = count_64k_blocks(off, apply);

        total_64k_writes += blocks;
        bytes_accounted  += apply;
        ++total_events;

        remaining -= apply;
    }

    if (remaining > 0) {
        std::cerr << "[WARN] trace가 충분하지 않아 target_bytes를 모두 채우지 못했습니다. "
                  << "accounted=" << bytes_accounted << " / target=" << target_bytes << " bytes\n";
    }

    std::cout << "Target bytes           : " << target_bytes << "\n"
              << "Accounted bytes        : " << bytes_accounted << "\n"
              << "Write events processed : " << total_events << "\n"
              << "64KiB-aligned writes   : " << total_64k_writes << "\n";

    return 0;
}