/**
 * trace_utilization_interruptible.cpp
 *
 * 빌드:
 *   g++ -O2 -std=c++17 -o trace_utilization trace_utilization_interruptible.cpp
 *
 * 사용:
 *   ./trace_utilization <trace.csv> <device_size_in_bytes> <parser_type>
 *
 * 예)
 *   ./trace_utilization alibaba.csv 1920383410176 alibaba
 *
 * 동작:
 *   - 긴 프리프로세스(파싱)가 진행되는 중에 Ctrl + C 를 누르면,
 *     그 시점까지 읽은 Write 구간으로 utilized bytes를 즉시 계산/출력하고 종료.
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
#include <csignal>
#include <atomic>
#include <cassert>
#include "trace_parser.h"

struct Range {
    uint64_t begin;   // inclusive
    uint64_t end;     // exclusive
};

/* ---------- 전역 상태 ---------- */
static std::vector<Range> g_ranges;
static uint64_t           g_dev_size = 0;
static std::string        g_trace_path;
static std::atomic<bool>  g_interrupted{false}; // SIGINT 플래그

/* 안전한 신호 처리기: 플래그만 세운다 */
static void on_sigint(int) {
    g_interrupted.store(true, std::memory_order_relaxed);
}

/* 현재까지 수집된 ranges로 utilized 바이트 계산 */
static uint64_t compute_utilized_bytes(const std::vector<Range>& ranges) {
    if (ranges.empty()) return 0;

    std::vector<Range> tmp = ranges; // 병합용 복사본
    std::sort(tmp.begin(), tmp.end(),
              [](const Range& a, const Range& b){ return a.begin < b.begin; });

    uint64_t utilized = 0;
    uint64_t cur_beg  = tmp[0].begin;
    uint64_t cur_end  = tmp[0].end;

    for (size_t i = 1; i < tmp.size(); ++i) {
        if (tmp[i].begin <= cur_end) {
            if (tmp[i].end > cur_end) cur_end = tmp[i].end;
        } else {
            utilized += (cur_end - cur_beg);
            cur_beg = tmp[i].begin;
            cur_end = tmp[i].end;
        }
    }
    utilized += (cur_end - cur_beg);
    return utilized;
}

/* 공통 출력 루틴 */
static void print_report(uint64_t utilized, uint64_t dev_size, const char* header = nullptr) {
    if (header) std::cout << header << "\n";
    double utilization_pct = dev_size ? (double)utilized * 100.0 / (double)dev_size : 0.0;

    std::cout << "Utilized bytes: " << utilized         << '\n'
              << "Device size   : " << dev_size         << '\n'
              << "Utilization   : " << utilization_pct  << " %\n";
}

int main(int argc, char* argv[]) {
    if (argc != 4) {
        std::cerr << "usage: " << argv[0] << " <trace.csv> <device_size_bytes> <parser_type>\n";
        return 1;
    }

    g_trace_path = argv[1];
    g_dev_size   = std::strtoull(argv[2], nullptr, 10);
    const std::string parser_type = argv[3];

    // SIGINT(Ctrl+C) 핸들러 등록
    std::signal(SIGINT, on_sigint);

    std::ifstream in(g_trace_path);
    if (!in) {
        perror("open trace");
        return 1;
    }

    g_ranges.reserve(1 << 20); // 초기 1M 라인 가정

    std::string line;
    int line_count = 0;
    ITraceParser* traceParser = createTraceParser(parser_type);

    while (std::getline(in, line)) {
        line_count++;

        // Ctrl+C 감지 → 즉시 그 시점까지 결과 출력 후 종료
        if (g_interrupted.load(std::memory_order_relaxed)) {
            uint64_t utilized_so_far = compute_utilized_bytes(g_ranges);
            print_report(utilized_so_far, g_dev_size, "[Interrupted] Partial result up to current line:");
            return 130; // 128+SIGINT 관례적으로 130
        }

        ParsedRow row = traceParser->parseTrace(line);
        if (row.op_type != "W" && row.op_type != "w" && row.op_type != "WS") continue; // Reads 건너뜀

        uint64_t offset = row.lba_offset;
        uint64_t size   = row.lba_size;

        if (size < 512) {
            // 너무 작은 write는 스킵 (원 코드 유지)
            // printf("skip! %d %s\n", line_count, line.c_str());
            continue;
        }
        if (size == 0) continue;

        // 범위 추가 (나중에 병합)
        g_ranges.push_back({offset, offset + size});

        // (선택) 매우 긴 트레이스의 경우 주기적으로도 체크 가능
        // if ((line_count & 0xFFFF) == 0 && g_interrupted.load(std::memory_order_relaxed)) { ... }
    }
    in.close();

    // 정상 종료 시 최종 결과 계산/출력
    if (g_ranges.empty()) {
        std::cout << "No write events found.\n";
        return 0;
    }

    uint64_t utilized = compute_utilized_bytes(g_ranges);
    print_report(utilized, g_dev_size);
    return 0;
}
