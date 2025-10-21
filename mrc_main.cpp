#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <stdexcept>
#include <iomanip>
#include <limits>
#include <map>
#include <cassert>
#include <signal.h>
#include <memory>
#include <boost/stacktrace.hpp>
#include <sys/mman.h>

#include "trace_parser.h"   // 제공된 파서 헤더
#include "mrc_calculator.h"

template <typename T>
static inline void dontdump_vec(std::vector<T>& v) {
#if defined(__linux__) && defined(MADV_DONTDUMP)
    if (!v.empty()) {
        void*  p   = static_cast<void*>(v.data());
        size_t len = v.size() * sizeof(T);
        (void)madvise(p, len, MADV_DONTDUMP);
    }
#endif
}

static void signal_handler(int signum) {
    std::cerr << "Received signal " << signum << ", stack trace:\n";
    boost::stacktrace::stacktrace st;
    for (std::size_t i = 0; i < st.size(); ++i) {
        const auto& f = st[i];
        const auto file = f.source_file();
        std::cerr << "#" << i << ' ' << f.name();
        if (!file.empty()) std::cerr << " at " << file << ':' << f.source_line();
        else               std::cerr << " at " << f.address();
        std::cerr << '\n';
    }
    ::signal(signum, SIG_DFL);
    ::raise(signum);
}

static void print_usage() {
    std::cerr << "Usage: ./mrc_analyzer --file <trace_file.csv> --type <LRU|OPT> "
                 "--capacity <capacity_bytes> [--H <num_buckets>] "
                 "[--interval <write_bytes>] "
                 "[--miss-out <outfile>] [--miss-append]\n";
    std::cerr << "  --file       : Path to the trace file.\n";
    std::cerr << "  --type       : Algorithm type (LRU or OPT).\n";
    std::cerr << "  --capacity   : Cache capacity in bytes.\n";
    std::cerr << "  --H          : Number of histogram buckets for MRC output (default: 1000).\n";
    std::cerr << "  --interval   : Compute MRC every <write_bytes> written (by write data size).\n";
    std::cerr << "  --miss-out   : Output CSV file path (default: stdout).\n";
    std::cerr << "  --miss-append: Append to output file.\n";
    std::cerr << "  --trace-type : Trace format type for parser (csv, blktrace).\n";
}

int main(int argc, char* argv[]) {
    std::string file_path;
    std::string algo_str;
    std::string trace_type;
    int num_buckets = 1000;
    uint64_t capacity_bytes = 0;

    // 새 옵션
    uint64_t interval_bytes = 0;

    std::string miss_out_path;
    bool miss_append = false;

    signal(SIGSEGV, signal_handler);
    signal(SIGABRT, signal_handler);
    signal(SIGFPE,  signal_handler);
    signal(SIGINT,  signal_handler);

    // 인자 파싱
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--file" && i + 1 < argc) {
            file_path = argv[++i];
        } else if (arg == "--type" && i + 1 < argc) {
            algo_str = argv[++i];
        } else if (arg == "--H" && i + 1 < argc) {
            num_buckets = std::stoi(argv[++i]);
        } else if (arg == "--capacity" && i + 1 < argc) {
            capacity_bytes = std::stoull(argv[++i]);
        } else if (arg == "--interval" && i + 1 < argc) {
            interval_bytes = std::stoull(argv[++i]);
        } else if (arg == "--miss-out" && i + 1 < argc) {
            miss_out_path = argv[++i];
        } else if (arg == "--miss-append" && i + 1 < argc) {
            miss_append = true;
        } else if (arg == "--trace-type" && i + 1 < argc) {
            trace_type = argv[++i];
        }
        else {
            std::cerr << "Unknown argument: " << arg << "\n";
            print_usage();
            return 1;
        }
    }
    assert(capacity_bytes > 0);

    if (file_path.empty() || algo_str.empty()) {
        std::cerr << "Error: --file and --type are required arguments.\n";
        print_usage();
        return 1;
    }

    AlgorithmType algo_type;
    if (algo_str == "LRU")      algo_type = AlgorithmType::LRU;
    else if (algo_str == "OPT") algo_type = AlgorithmType::OPT;
    else {
        std::cerr << "Error: Invalid algorithm type '" << algo_str << "'. Choose 'LRU' or 'OPT'.\n";
        print_usage();
        return 1;
    }

    if (num_buckets <= 0) {
        std::cerr << "Error: Number of buckets (--H) must be positive.\n";
        return 1;
    }

    std::ifstream trace_file(file_path);
    if (!trace_file.is_open()) {
        std::cerr << "Error: Could not open file " << file_path << "\n";
        return 1;
    }

    // 2) 트레이스 파싱 (W/WS만 블록화)
    constexpr uint64_t BLOCK_SIZE = 4096; // 4KB
    std::vector<long long> trace;
    std::string line;

    std::cout << "Parsing and blockifying trace file...\n";
    printf("trace_type = %s\n", trace_type.c_str());
    std::unique_ptr<ITraceParser> parser(createTraceParser(trace_type));
    uint64_t byte_limit = 20000000000000ULL; // 10TB (주의: 0 하나 줄임)
    uint64_t written_bytes = 0ULL;
    uint64_t next_print_written_bytes = 100000000000ULL; // 100GB
    bool break_flag = false;

    while (std::getline(trace_file, line)) {
        ParsedRow parsed = parser->parseTrace(line);
        //printf("parsed.op_type: %s\n", parsed.op_type.c_str());
        //printf("line %s \n", line.c_str());
        if (parsed.op_type == "W" || parsed.op_type == "WS") {
            if (parsed.lba_size <= 0) continue;

            uint64_t start_block = parsed.lba_offset / BLOCK_SIZE;
            uint64_t end_block   = (parsed.lba_offset + parsed.lba_size - 1) / BLOCK_SIZE;

            for (uint64_t block = start_block; block <= end_block; ++block) {
                trace.push_back(static_cast<long long>(block));
                written_bytes += BLOCK_SIZE;
                if (written_bytes >= byte_limit) { break_flag = true; break; }
            }

            if (written_bytes >= next_print_written_bytes) {
                std::cout << "Parsing Written bytes: " << (written_bytes / 1000000000ULL)
                          << " GB\n";
                next_print_written_bytes += 100000000000ULL; // +100GB
            }
        }
        if (break_flag) {
            std::cout << "Reached byte limit of 10TB, stopping trace parsing.\n";
            break;
        }
    }
    dontdump_vec(trace);
    std::cout << "Total block accesses to analyze: " << trace.size() << "\n";
    if (trace.empty()) {
        std::cout << "No valid 'W' or 'WS' operations found in the trace.\n";
        return 0;
    }

    // 3) 체크포인트(구간) 계산: interval_bytes→interval_blocks
    std::vector<size_t> checkpoints; // 각 원소 = prefix 길이(액세스 수)
    if (interval_bytes > 0) {
        uint64_t blocks_per_interval =
            (interval_bytes + BLOCK_SIZE - 1) / BLOCK_SIZE; // 올림
        if (blocks_per_interval == 0) blocks_per_interval = 1;
        for (uint64_t acc = blocks_per_interval; acc <= trace.size(); acc += blocks_per_interval) {
            checkpoints.push_back(static_cast<size_t>(acc));
        }
        // 마지막이 딱 안 맞으면 끝도 포함
        if (checkpoints.empty() || checkpoints.back() != trace.size())
            checkpoints.push_back(trace.size());
    } else {
        checkpoints.push_back(trace.size()); // 전체만
    }

    // 4) MRC 계산 (구간 단위)
    std::cout << "Calculating MRC (" << algo_str << "), H=" << num_buckets
              << ", intervals=" << checkpoints.size() << "...\n";

    MrcCalculator calculator;

    // 축의 최대 캐시 크기(블록)는 기존 코드와 동일하게 capacity 기준 유지
    long long axis_dataset_size_blocks = static_cast<long long>(capacity_bytes / BLOCK_SIZE);

    auto series = calculator.calculate_mrc_intervals(
        trace, axis_dataset_size_blocks, algo_type, num_buckets, checkpoints);

    // 5) 출력
    std::ostream* out = &std::cout;
    std::unique_ptr<std::ofstream> fout;
    if (!miss_out_path.empty()) {
        std::ios_base::openmode mode = std::ios::out | (miss_append ? std::ios::app : std::ios::trunc);
        fout = std::make_unique<std::ofstream>(miss_out_path, mode);
        if (!fout->is_open()) {
            std::cerr << "Error: Could not open output file " << miss_out_path << "\n";
            return 1;
        }
        out = fout.get();
    }

    (*out) << "IntervalBytes,CacheSize(blocks),MissRate(%)\n";
    (*out) << std::fixed << std::setprecision(4);

    for (const auto& item : series) {
        const size_t prefix_len = item.first; // 액세스 수
        const uint64_t interval_b = static_cast<uint64_t>(prefix_len) * BLOCK_SIZE;

        for (const auto& kv : item.second) {
            (*out) << interval_b << "," << kv.first << "," << (kv.second * 100.0) << "\n";
        }
    }
    return 0;
}
