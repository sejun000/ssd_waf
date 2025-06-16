/********************************************************************
 * trace_utilization_mt.cpp  (multi-threaded, 40-core ready)
 *
 * 빌드:
 *   g++ -O3 -std=c++17 -fopenmp -D_GLIBCXX_PARALLEL \
 *       -o trace_utilization_mt trace_utilization_mt.cpp
 *
 * 실행:
 *   OMP_NUM_THREADS=40 ./trace_utilization_mt <trace.csv> <device_bytes>
 *
 ********************************************************************/
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <vector>
#include <iostream>
#include <algorithm>
#include <parallel/algorithm>   // __gnu_parallel::sort
#include <omp.h>

struct Range { uint64_t b, e; };     // [b, e)

static inline bool is_newline(char c) { return c == '\n' || c == '\r'; }

/* ────────────── 한 줄 파싱: CSV 5 열 고정 ────────────── */
static inline void parse_line(const char* s, const char* end,
                              std::vector<Range>& out)
{
    /* 쉼표 인덱스 0~4 찾기 */
    const char* comma[5];
    int idx = 0;
    for (const char* p = s; p < end && idx < 5; ++p)
        if (*p == ',') comma[idx++] = p;
    if (idx < 4) return;                   // 형식 오류

    char type = comma[0][1];
    if (type != 'W' && type != 'w') return;

    uint64_t off  = strtoull(comma[1] + 1, nullptr, 10);
    uint64_t size = strtoull(comma[2] + 1, nullptr, 10);
    if (size == 0) return;

    out.push_back({off, off + size});
}

/* ────────────── main ────────────── */
int main(int argc, char* argv[])
{
    if (argc != 3) {
        std::cerr << "usage: " << argv[0] << " <trace.csv> <device_size_bytes>\n";
        return 1;
    }
    const char* path = argv[1];
    uint64_t dev_size = strtoull(argv[2], nullptr, 10);

    /* 0) mmap 으로 파일 통째 매핑 */
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror("open"); return 1; }
    struct stat st{};
    if (fstat(fd, &st) < 0) { perror("fstat"); return 1; }
    size_t fsz = st.st_size;
    if (fsz == 0) { std::cerr << "empty file\n"; return 1; }

    char* base = static_cast<char*>(mmap(nullptr, fsz, PROT_READ, MAP_PRIVATE, fd, 0));
    if (base == MAP_FAILED) { perror("mmap"); return 1; }

    /* 1) 스레드별 구간 계산 */
    const int T = omp_get_max_threads();          // OMP_NUM_THREADS
    std::vector<std::vector<Range>> buckets(T);

#pragma omp parallel
    {
        int tid = omp_get_thread_num();
        size_t chunk = fsz / T;
        size_t start = tid * chunk;
        size_t stop  = (tid == T - 1) ? fsz : (tid + 1) * chunk;

        /* 라인이 끊기지 않도록 경계 조정 */
        if (tid)       while (start < fsz && !is_newline(base[start-1])) ++start;
        if (tid < T-1) while (stop  < fsz && !is_newline(base[stop]))    ++stop;

        auto& vec = buckets[tid];
        vec.reserve(1 << 18);                     // 경험적 초기값

        /* 2) 구간 스캔 */
        const char* p = base + start;
        const char* end = base + stop;
        const char* line = p;
        for (; p < end; ++p) {
            if (*p == '\n') {
                parse_line(line, p, vec);
                line = p + 1;
            }
        }
        if (line < end) parse_line(line, end, vec);   // 마지막 줄
    }

    munmap(base, fsz);
    close(fd);

    /* 3) 벡터 합치기 */
    std::vector<Range> ranges;
    size_t total_reserve = 0;
    for (auto& v : buckets) total_reserve += v.size();
    ranges.reserve(total_reserve);
    for (auto& v : buckets) {
        ranges.insert(ranges.end(),
                      std::make_move_iterator(v.begin()),
                      std::make_move_iterator(v.end()));
        v.clear();
    }

    if (ranges.empty()) {
        std::cout << "No write events found.\n";
        return 0;
    }

    /* 4) 병렬 정렬 (GNU parallel mode) */
    __gnu_parallel::sort(ranges.begin(), ranges.end(),
                         [](const Range& a, const Range& b){ return a.b < b.b; });

    /* 5) 병합 */
    uint64_t used = 0;
    uint64_t cur_b = ranges[0].b;
    uint64_t cur_e = ranges[0].e;
    for (size_t i = 1; i < ranges.size(); ++i) {
        if (ranges[i].b <= cur_e)
            cur_e = std::max(cur_e, ranges[i].e);
        else {
            used += cur_e - cur_b;
            cur_b = ranges[i].b;
            cur_e = ranges[i].e;
        }
    }
    used += cur_e - cur_b;
    if (used > dev_size) used = dev_size;   // 안전벨트

    double pct = 100.0 * static_cast<long double>(used) / dev_size;
    std::cout << "Utilized bytes: " << used     << '\n'
              << "Device size   : " << dev_size << '\n'
              << "Utilization   : " << pct      << " %\n";
    return 0;
}
