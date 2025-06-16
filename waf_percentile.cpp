// inter_write_gap.cpp  (overlap-aware, page granularity)
//
// build:  g++ -O2 -std=c++17 -o inter_write_gap inter_write_gap.cpp
// usage:  ./inter_write_gap trace.csv [bins]

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <string>
#include <unordered_map>
#include <vector>
#include "trace_parser.h"

constexpr uint64_t kPage = 4096;                // 페이지 크기 (4 KiB)

// ---------- 퍼센타일 헬퍼 ------------------------------------------------------
double pct_val(const std::vector<uint64_t>& v, double p) {
    if (v.empty()) return 0;
    double idx  = (p / 100.0) * (v.size() - 1);
    size_t lo   = static_cast<size_t>(std::floor(idx));
    size_t hi   = static_cast<size_t>(std::ceil(idx));
    double frac = idx - lo;
    return v[lo] + frac * (v[hi] - v[lo]);
}
// -----------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    if (argc < 4) {
        std::cerr << "usage: " << argv[0] << " trace.csv [trace] [gaps.txt] [percentile.txt] [number of percentile]\n";
        return 1;
    }
    const std::string trace_path = argv[1];
    const std::string gaps_txt   = argv[2];
    const std::string percentile_txt = argv[3];
    const size_t bins = (argc >= 5) ? std::stoul(argv[4]) : 100;

    std::ifstream fin(trace_path);
    if (!fin) { std::cerr << "cannot open " << trace_path << '\n'; return 1; }
    std::unordered_map<uint64_t, uint64_t> last_B;   // page → 마지막 B
    std::vector<uint64_t> gaps;   gaps.reserve(1 << 20);
    uint64_t total_B = 0;
    std::string line;
    // write limit will be set to 15000 GiB
    uint64_t write_limit = 15000ULL * 1024ULL * 1024ULL * 1024ULL; // 15000 GiB
    uint64_t write_bytes = 0;
    while (std::getline(fin, line)) {
        // 쉼표 위치만 빠르게 찾는다 (컬럼 5개)
        ITraceParser* parser = createTraceParser("csv");
        // 컬럼 경계(쉼표) 4개만 찾으면 충분
        ParsedRow row = parser->parseTrace(line);

        std::string op = row.op_type;
        if (op != "W" && op != "WS") continue;                 // write 이외 스킵
        if (write_bytes >= write_limit) break;               // 15000 GiB 초과 시 종료

        uint64_t off  = row.lba_offset;
        uint64_t size = row.lba_size;
        write_bytes += size;

        uint64_t first_pg = off / kPage;
        uint64_t npages   = (size + kPage - 1) / kPage;   // ceil

        for (uint64_t pg = first_pg; pg < first_pg + npages; ++pg) {
            auto it = last_B.find(pg);
            if (it != last_B.end())
                gaps.push_back(total_B - it->second);
            last_B[pg] = total_B;
        }
        total_B += size;
    }
    fin.close();

    if (gaps.empty()) {
        std::cerr << "no gap samples.\n";  return 0;
    }
    std::sort(gaps.begin(), gaps.end());

    // ---------- 요약 통계 -----------------------------------------------------
    auto pr = [](const char* t, uint64_t v){ std::cout << t << v << " B\n"; };
    std::cout.imbue(std::locale(""));
    pr("samples : ", gaps.size());
    pr("min     : ", gaps.front());
    pr("p50     : ", static_cast<uint64_t>(pct_val(gaps,50)));
    pr("p90     : ", static_cast<uint64_t>(pct_val(gaps,90)));
    pr("p95     : ", static_cast<uint64_t>(pct_val(gaps,95)));
    pr("p99     : ", static_cast<uint64_t>(pct_val(gaps,99)));
    pr("max     : ", gaps.back());

    // (1) gaps.txt ─ raw
    { std::ofstream f(gaps_txt);
      for (uint64_t g: gaps) f << g << '\n'; }

    // (2) percentiles.txt ─ p1~p100
    { std::ofstream f(percentile_txt);
      f << "#p  bytes\n";
      for (int p = 1; p <= 100; ++p)
          f << p << ' ' << std::fixed << std::setprecision(0)
            << pct_val(gaps, p) << '\n'; }

    // (3) hist.txt ─ log-space 히스토그램
    double minLog = std::log10(static_cast<double>(gaps.front()));
    double maxLog = std::log10(static_cast<double>(gaps.back()));
    std::vector<uint64_t> hist(bins, 0);
    for (uint64_t g : gaps) {
        double pos = (std::log10(static_cast<double>(g)) - minLog) /
                     (maxLog - minLog);
        size_t bin = std::min(bins - 1, static_cast<size_t>(pos * bins));
        ++hist[bin];
    }
    { std::ofstream f("hist.txt");
      for (size_t i = 0; i < bins; ++i) {
          double upper = std::pow(10.0,
              minLog + (static_cast<double>(i + 1) / bins) * (maxLog - minLog));
          f << std::fixed << std::setprecision(0) << upper << ' ' << hist[i] << '\n';
      } }

    std::cout << "wrote gaps.txt, hist.txt (" << bins
              << " bins) and percentiles.txt (p1-p100)\n";
    return 0;
}
