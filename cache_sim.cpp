#include "cache_sim.h"
#include "trace_parser.h"
#include "icache.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <map>
#include <tuple>   // std::tuple
#include <signal.h>
#include <execinfo.h>
#include <boost/stacktrace.hpp>
#include <iostream>
#include <cstring>
#include <memory>

void signal_handler(int signum) {
    std::cerr << "Received signal " << signum << ", stack trace:\n";

    boost::stacktrace::stacktrace st;
    for (std::size_t i = 0; i < st.size(); ++i) {
        const auto& f   = st[i];
        const auto file = f.source_file();   // std::string

        std::cerr << "#" << i << ' ' << f.name();

        if (!file.empty()) {                 // 문자열 비었는지 확인
            std::cerr << " at " << file
                      << ':' << f.source_line();
        } else {
            std::cerr << " at " << f.address();
        }
        std::cerr << '\n';
    }
    exit(signum);
}

// LRU 정책: 주어진 lba 범위의 블록들을 캐시에 추가
void issue_op_to_cache(ICache& cache, long long lba_offset, int lba_size, OP_TYPE op_type) {
    int block_size = cache.get_block_size();
    long start_block = static_cast<long>(lba_offset / block_size);
    long end_block = static_cast<long>((lba_offset + lba_size) / block_size);
    long long req_start = lba_offset;
    long long req_end = lba_offset + lba_size;
    std::map<long, int> newBlocks;
    for (long block = start_block; block <= end_block; block++) {
        long long block_start = static_cast<long long>(block) * block_size;
        long long block_end = block_start + block_size;    
        long long left_offset = std::max(block_start, req_start);
        long long right_offset = std::min(block_end, req_end);
        if (right_offset <= left_offset) {
            continue;
        }
        newBlocks[block] = right_offset - left_offset;
    }
    cache.batch_insert(0, newBlocks, op_type);
}

// Read/Write hit ratio 계산 (퍼센트)
void calc_hit_ratio(long long read_hit_size, long long total_read_size,
                    long long write_hit_size, long long total_write_size,
                    double &read_hit_ratio, double &write_hit_ratio) {
    read_hit_ratio = (total_read_size > 0) ? (static_cast<double>(read_hit_size) / total_read_size) * 100 : 0;
    write_hit_ratio = (total_write_size > 0) ? (static_cast<double>(write_hit_size) / total_write_size) * 100 : 0;
}

// =======================
// main() 함수
// =======================

void print_stats(bool intermeidate, long long total_read, long long total_write, long long total_read_size, long long total_write_size, long long read_hit_size, long long write_hit_size, long long cache_write_size, long long cold_tier_write_size, long long cold_tier_read_size, long max_cache_blocks, size_t cache_size) {
    double final_read_hit_ratio, final_write_hit_ratio;
    calc_hit_ratio(read_hit_size, total_read_size, write_hit_size, total_write_size, final_read_hit_ratio, final_write_hit_ratio);
    
    if (intermeidate) {
        std::cout << "\nIntermediate Stats" << std::endl;
    }
    else {
        std::cout << "\nFinal Stats" << std::endl;
    }
    std::cout << "\nCurrent Cache Hit Ratios:" << std::endl;
    //std::cout << "Read Cache Hit Ratio: " << final_read_hit_ratio << "%" << std::endl;
    //std::cout << "Write Cache Hit Ratio: " << final_write_hit_ratio << "%" << std::endl;
    //std::cout << "total_read = " << total_read << ", total_write = " << total_write << std::endl;
    //std::cout << "total_read_bytes = " << total_read_size << ", total_write_bytes = " << total_write_size << std::endl;
    std::cout << "cache size = " << max_cache_blocks << std::endl;
    std::cout << "current cache size = " << cache_size << std::endl;
    std::cout << "cache_write_size = " << cache_write_size << std::endl;
    std::cout << "cold_tier_write_size = " << cold_tier_write_size << std::endl;
    //std::cout << "cold_tier_read_size = " << cold_tier_read_size << std::endl;
}

int main(int argc, char* argv[]) {
    signal(SIGSEGV, signal_handler);
    signal(SIGABRT, signal_handler);
    signal(SIGFPE, signal_handler);
    signal(SIGINT, signal_handler);
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " trace_file cache_size [--block_size N] [--rw_policy all|write-only] [--trace_format csv|blktrace] [--cache_policy LRU/FIFO] [--cache_trace] [--cold_capacity [bytes]] [--waf_log_file [filename]] [--valid_ratio [%]] [--stat_log_file [filename]]" << std::endl;
        return 1;
    }
    std::string trace_file = argv[1];
    long cache_size = std::stol(argv[2]);
    //int block_size = 65536; // 기본 블록 크기
    int block_size = 4096;
    std::string policy = "all";
    std::string trace_format = "csv";
    std::string cache_trace_output = "";
    std::string cold_trace_output = "";
    std::string cache_policy = "LRU";
    std::string waf_log_file = "";
    std::string stat_log_file = "";
    double valid_ratio = 0.0;
    bool cache_trace = false;
    uint64_t cold_capacity = 0;

    // 추가 인자 파싱
    for (int i = 3; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--block_size" && i + 1 < argc) {
            block_size = std::stoi(argv[++i]);
        } else if (arg == "--rw_policy" && i + 1 < argc) {
            policy = argv[++i];
        } else if (arg == "--trace_format" && i + 1 < argc) {
            trace_format = argv[++i];
        } else if (arg == "--cache_policy" && i + 1 < argc) {
            cache_policy = argv[++i];
        } else if (arg == "--cache_trace" && i + 1 < argc) {
            cache_trace_output = argv[++i];
            cache_trace = true;
        } else if (arg == "--cold_trace" && i + 1 < argc) {
            cold_trace_output = argv[++i];
        } else if (arg == "--cold_capacity" && i + 1 < argc) {
            // cold_capacity는 바이트 단위로 입력받음
            cold_capacity = std::stoll(argv[++i]); 
        } else if (arg == "--waf_log_file" && i + 1 < argc) {
            waf_log_file = argv[++i];
        } else if (arg == "--valid_ratio" && i + 1 < argc) {
            valid_ratio = std::stod(argv[++i]);
        } else if (arg == "--stat_log_file" && i + 1 < argc) {
             stat_log_file = argv[++i];
        }
        else {
            std::cerr << "Unknown argument: " << arg << std::endl;
            return 1;
        }
    }
    // printf parameter
    printf("trace_file = %s\n", trace_file.c_str());
    printf("cache_size = %ld\n", cache_size);
    printf("block_size = %d\n", block_size);
    printf("policy = %s\n", policy.c_str());
    printf("trace_format = %s\n", trace_format.c_str());
    printf("cache_policy = %s\n", cache_policy.c_str());
    printf("cache_trace_output = %s\n", cache_trace_output.c_str());
    printf("cold_trace_output = %s\n", cold_trace_output.c_str());
    printf("cold_capacity = %lu\n", cold_capacity);
    assert (cold_capacity > 0);
    // Factory 함수를 이용해 적절한 TraceParser 생성
    ITraceParser* parser = createTraceParser(trace_format);
    long max_cache_blocks = cache_size / block_size;
    printf("max_cache_blocks = %ld\n", max_cache_blocks);
    std::unique_ptr<ICache> cache(createCache(cache_policy, max_cache_blocks, cold_capacity, block_size, cache_trace, cache_trace_output, cold_trace_output, waf_log_file, valid_ratio, stat_log_file));
    // 통계 변수 초기화
    long long total_read = 0, total_write = 0;
    long long total_read_size = 0, total_write_size = 0;
    long long read_hit_size = 0, write_hit_size = 0;
    long long cache_write_size = 0, cold_tier_write_size = 0, cold_tier_read_size = 0;
    
    std::ifstream infile(trace_file);
    if (!infile) {
        std::cerr << "File Error" << std::endl;
        std::cerr << "Cannot open file: " << trace_file << std::endl;
        return 1;
    }
    
    std::string line;
    long long line_count = 0;
    const long long line_count_limit = 270000000000000000ULL;
    const long long cache_write_size_limit = 40000000000000ULL;
    
    while (std::getline(infile, line) && line_count < line_count_limit) {
        line_count++;
        if (line_count % 1000000 == 0) {
            print_stats(true, total_read, total_write, total_read_size, total_write_size, read_hit_size, write_hit_size, cache_write_size, cold_tier_write_size, cold_tier_read_size, max_cache_blocks, cache->size());
        }
        cache->print_stats();
        if (cache_write_size > cache_write_size_limit) {
            break;
        }
        // 사용자 구현 parse_trace 함수 호출
        ParsedRow parsed = parser->parseTrace(line);
        // printf ("parsed.dev_id = %s, parsed.op_type = %s, parsed.lba_offset = %lld, parsed.lba_size = %d, parsed.timestamp = %f\n", parsed.dev_id.c_str(), parsed.op_type.c_str(), parsed.lba_offset, parsed.lba_size, parsed.timestamp);
        if (parsed.dev_id.empty()) {
            continue;
        }
        long long write_bytes_to_cache;
        long long evicted_blocks;
        if (parsed.op_type == "R" || parsed.op_type == "RS") {
            std::tie(write_bytes_to_cache, evicted_blocks, write_hit_size) = cache->get_status();
            //if (cache.is_cache_filled()) {
                total_read++;
                total_read_size += parsed.lba_size;
            //}
            if (policy == "all" || policy == "read-only") {
                issue_op_to_cache(*cache, parsed.lba_offset, parsed.lba_size, OP_TYPE::READ);
            }
        } else if (parsed.op_type == "W" || parsed.op_type == "WS") {
            std::tie(write_bytes_to_cache, evicted_blocks, write_hit_size) = cache->get_status();
            
            //if (cache.is_cache_filled()) {
                total_write++;
                total_write_size += parsed.lba_size;
                cache_write_size = write_bytes_to_cache;
                cold_tier_write_size = block_size * evicted_blocks;
            //}
            if (policy == "all" || policy == "write-only") {
                issue_op_to_cache(*cache, parsed.lba_offset, parsed.lba_size, OP_TYPE::WRITE);
            }
            if (policy == "write-only") {
             //   cache->print_cache_trace(parsed.lba_offset, parsed.lba_size, OP_TYPE::WRITE);
            }

        }
    }
    
    double final_read_hit_ratio, final_write_hit_ratio;
    calc_hit_ratio(read_hit_size, total_read_size, write_hit_size, total_write_size, final_read_hit_ratio, final_write_hit_ratio);
    
    print_stats(false, total_read, total_write, total_read_size, total_write_size, read_hit_size, write_hit_size, cache_write_size, cold_tier_write_size, cold_tier_read_size, max_cache_blocks, cache->size());
    cache->print_stats();
    return 0;
}
