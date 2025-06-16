/**
 * trace_utilization.cpp
 *
 * 빌드:
 *   g++ -O2 -std=c++17 -o trace_utilization trace_utilization.cpp
 *
 * 사용:
 *   ./trace_utilization <trace.csv> <device_size_in_bytes>
 *
 * 예)
 *   ./trace_utilization alibaba.csv 1920383410176
 *
 * 형식 (CSV, 0-base 열 번호)
 *   0: 디스크 ID (무시)
 *   1: 이벤트 문자 (W 또는 R)
 *   2: LBA 오프셋  [bytes]
 *   3: LBA 크기   [bytes]
 *   4: 타임스탬프 (무시)
 *
 * 출력:
 *   Utilized bytes: X
 *   Device size   : Y
 *   Utilization   : Z %
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>
#include <string>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <cassert>
#include "trace_parser.h"

struct Range {
    uint64_t begin;   // inclusive
    uint64_t end;     // exclusive
};

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "usage: " << argv[0] << " <trace.csv> <device_size_bytes>\n";
        return 1;
    }



    const char* path        = argv[1];
    const uint64_t dev_size = std::strtoull(argv[2], nullptr, 10);

    std::ifstream in(path);
    if (!in) {
        perror("open trace");
        return 1;
    }

    std::vector<Range> ranges;
    ranges.reserve(1 << 20);          // 초기 1 M 라인 가정 (필요시 자동 증가)

    std::string line;
    std::string prev_line;
    int line_count = 0;
    ITraceParser* traceParser = createTraceParser("csv");
    while (std::getline(in, line)) {
        line_count++;
       
        ParsedRow row = traceParser->parseTrace(line);
        if (row.op_type != "W" && row.op_type != "w") continue;   // R 은 건너뜀

        uint64_t offset = row.lba_offset;
        uint64_t size   = row.lba_size;
        if (row.lba_size < 512) {
            //printf("offset %ld size %ld\n", row.lba_offset, row.lba_size);
            printf("skip! %d %s\n ", line_count, line.c_str());
            continue;
            //printf ("%ld %ld %ld %s\n", ranges[ranges.size()-1].begin, ranges[ranges.size()-1].end, prev_line.size(), prev_line.c_str());
            //printf ("%ld %ld %s %ld %d\n", offset, size, line.c_str(), line.size(), idx);
        }
        //assert (offset + size != 425666588672);
        if (size == 0) continue;

        ranges.push_back({offset, offset + size});
        prev_line = line;
    }
    in.close();

    if (ranges.empty()) {
        std::cout << "No write events found.\n";
        return 0;
    }

    /* 1) 오프셋 기준 정렬 → 2) 겹치는 구간 병합 */
    std::sort(ranges.begin(), ranges.end(),
              [](const Range& a, const Range& b){ return a.begin < b.begin; });

    uint64_t utilized = 0;
    uint64_t cur_beg  = ranges[0].begin;
    uint64_t cur_end  = ranges[0].end;

    for (size_t i = 1; i < ranges.size(); ++i) {
        if (ranges[i].begin <= cur_end) {
            cur_end = std::max(cur_end, ranges[i].end);            // 겹침
            if (cur_end == 425666588672) {
                printf("lba %ld size %ld \n", ranges[i].begin, ranges[i].end);
                assert(0);
            }
        } else {
            utilized += cur_end - cur_beg;                         // 확정
            cur_beg = ranges[i].begin;
            cur_end = ranges[i].end;
            //printf("lba %ld size %ld utilized %ld \n", ranges[i].begin, ranges[i].end, utilized);
        }
    }  
    utilized += cur_end - cur_beg;                                 // 마지막 구간 추가
    printf("%ld %ld %ld %ld\n", cur_beg, cur_end, ranges[ranges.size()-1].begin, ranges[ranges.size()-1].end);

    double utilization_pct = (double)utilized * 100.0 / (double)dev_size;

    std::cout << "Utilized bytes: " << utilized      << '\n'
              << "Device size   : " << dev_size      << '\n'
              << "Utilization   : " << utilization_pct << " %\n";
    return 0;
}
