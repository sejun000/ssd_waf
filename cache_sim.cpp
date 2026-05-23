#include "cache_sim.h"
#include "trace_parser.h"
#include "icache.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <map>
#include <unordered_map>
#include <tuple>   // std::tuple
#include <signal.h>
#include <execinfo.h>
#include <boost/stacktrace.hpp>
#include <iostream>
#include <cstring>
#include <memory>
#include <climits>

static uint64_t CACHE_WRITE_SIZE_LIMIT = 14ULL * 1024 * 1024 * 1024 * 1024; // 14 TB (default, override via --cache_write_size_limit)
static uint64_t COLD_WRITE_SIZE_LIMIT  = 0; // 0 = disabled; override via --cold_write_size_limit
static uint64_t PREFILL_LOG_INTERVAL   = CACHE_WRITE_SIZE_LIMIT / 100;
#define PREFILL_RATE (0.8)

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

// Fill only unique address ranges observed in the trace up to a byte limit; aligns to block/sector.
static void trace_prefill(ICache& cache,
                          const std::string& trace_file,
                          ITraceParser& parser,
                          uint64_t byte_limit,
                          int block_size, uint64_t cold_capacity) {
    const uint64_t align_unit = std::max<uint64_t>(block_size, SECTOR_SIZE);
    const uint64_t aligned_limit = (byte_limit / align_unit) * align_unit;
    uint64_t target_bytes = static_cast<uint64_t>(cold_capacity * PREFILL_RATE);
    target_bytes = (target_bytes / align_unit) * align_unit;
    if (aligned_limit == 0 || target_bytes == 0) {
        std::cout << "[prefill] skip: byte_limit too small\n";
        return;
    }
    target_bytes = std::min<uint64_t>(aligned_limit, target_bytes);

    std::ifstream infile(trace_file);
    if (!infile) {
        std::cerr << "[prefill] cannot open trace: " << trace_file << std::endl;
        return;
    }

    std::map<uint64_t, uint64_t> ranges; // start -> end (exclusive), aligned
    uint64_t unique_bytes = 0;
    std::string line;
    uint64_t next_scan_log = PREFILL_LOG_INTERVAL;
    uint64_t accumulated_bytes = 0;

    auto add_range = [&](uint64_t start, uint64_t end) {
        if (start >= end) return;
        uint64_t new_start = start;
        uint64_t new_end   = end;
        uint64_t removed   = 0;

        auto it = ranges.lower_bound(start);
        if (it != ranges.begin()) {
            auto prev = std::prev(it);
            if (prev->second >= start) {
                new_start = prev->first;
                new_end   = std::max(new_end, prev->second);
                removed  += prev->second - prev->first; // entire erased interval was already counted
                it = ranges.erase(prev);
            }
        }
        while (it != ranges.end() && it->first <= new_end) {
            new_end   = std::max(new_end, it->second);
            removed  += it->second - it->first; // entire erased interval was already counted
            it        = ranges.erase(it);
        }

        ranges[new_start] = new_end;
        uint64_t added = new_end - new_start;
        if (added >= removed) {
            unique_bytes += (added - removed);
        }
        while (unique_bytes >= next_scan_log) {
            std::cout << "[prefill][scan] covered " << (next_scan_log >> 30) << " GB of unique address space, accumulate = " << (accumulated_bytes >> 30) << " GB" << std::endl;
            next_scan_log += PREFILL_LOG_INTERVAL;
        }
    };
    
    while (std::getline(infile, line) && unique_bytes < target_bytes) {
        ParsedRow parsed = parser.parseTrace(line);
        if (parsed.dev_id.empty()) continue;
        uint64_t aligned_size = (static_cast<uint64_t>(parsed.lba_size) / align_unit) * align_unit;
        if (aligned_size == 0) continue;
        uint64_t aligned_offset = (static_cast<uint64_t>(parsed.lba_offset) / align_unit) * align_unit;

        uint64_t end = aligned_offset + aligned_size;
        add_range(aligned_offset, end);
        accumulated_bytes += aligned_size;
        if (accumulated_bytes >= aligned_limit) {
            break;
        }
    }

    uint64_t written = 0;
    uint64_t next_log = PREFILL_LOG_INTERVAL;
    const uint64_t fill_chunk = std::max<uint64_t>(block_size, align_unit);

    for (const auto& kv : ranges) {
        uint64_t start = kv.first;
        uint64_t end   = kv.second;
        if (written >= target_bytes) break;
        uint64_t len   = end - start;
        uint64_t remaining = target_bytes - written;
        len = std::min<uint64_t>(len, remaining);

        uint64_t offset = start;
        uint64_t left   = len;
        while (left > 0) {
            uint64_t chunk = std::min<uint64_t>(fill_chunk, left);
            issue_op_to_cache(cache, static_cast<long long>(offset), static_cast<int>(chunk), OP_TYPE::WRITE);
            offset  += chunk;
            left    -= chunk;
            written += chunk;

            if (written >= next_log) {
                std::cout << "[prefill][write] written " << (next_log >> 30) << " GB" << std::endl;
                next_log += PREFILL_LOG_INTERVAL;
            }
        }
    }

    std::cout << "[prefill] done, total " << written << " bytes (target " << target_bytes << ", align " << align_unit << ")" << std::endl;
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
        std::cerr << "Usage: " << argv[0] << " trace_file cache_size [--block_size N] [--rw_policy all|write-only] [--trace_format csv|blktrace] [--cache_policy LRU/FIFO] [--cache_trace] [--cold_capacity [bytes]] [--waf_log_file [filename]] [--valid_ratio [%]] [--stat_log_file [filename]] [--no_fill]" << std::endl;
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
    double periodic_ratio = 2.88;
    double util_step = 0.02;
    std::string moving_avg_type = "ewma";
    double moving_avg_window = 0.0;   // 0 → use LogCache default
    int gs_decision_period_segs = 8;  // GS-only knob; auto-derives util_step + ghost shadow size
    bool cache_trace = false;
    bool no_fill = true;
    uint64_t cold_capacity = 0;
    int lba_scale = 1;
    bool remap_lba = false;
    bool loop_trace = false;
    long long min_trace_loops = 0;

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
        } else if (arg == "--no_fill" || arg == "-no_fill" || arg == "-no_filll") {
            no_fill = true;
        } else if (arg == "--scale" && i + 1 < argc) {
            lba_scale = std::stoi(argv[++i]);
        } else if (arg == "--periodic_ratio" && i + 1 < argc) {
            periodic_ratio = std::stod(argv[++i]);
        } else if (arg == "--util_step" && i + 1 < argc) {
            util_step = std::stod(argv[++i]);
        } else if (arg == "--moving_avg_type" && i + 1 < argc) {
            moving_avg_type = argv[++i];
        } else if (arg == "--moving_avg_window" && i + 1 < argc) {
            moving_avg_window = std::stod(argv[++i]);
        } else if (arg == "--gs_decision_period_segs" && i + 1 < argc) {
            gs_decision_period_segs = std::stoi(argv[++i]);
        } else if (arg == "--cache_write_size_limit" && i + 1 < argc) {
            CACHE_WRITE_SIZE_LIMIT = std::stoull(argv[++i]);
            PREFILL_LOG_INTERVAL   = CACHE_WRITE_SIZE_LIMIT / 100;
        } else if (arg == "--remap_lba") {
            remap_lba = true;
        } else if (arg == "--cold_write_size_limit" && i + 1 < argc) {
            COLD_WRITE_SIZE_LIMIT = std::stoull(argv[++i]);
        } else if (arg == "--loop_trace") {
            loop_trace = true;
        } else if (arg == "--min_trace_loops" && i + 1 < argc) {
            min_trace_loops = std::stoll(argv[++i]);
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
    printf("lba_scale = %d\n", lba_scale);
    printf("periodic_ratio = %.2f\n", periodic_ratio);
    printf("util_step = %.4f\n", util_step);
    printf("moving_avg_type = %s\n", moving_avg_type.c_str());
    printf("moving_avg_window = %.0f blocks\n", moving_avg_window);
    printf("gs_decision_period_segs = %d\n", gs_decision_period_segs);
    printf("cache_write_size_limit = %lu bytes (%.2f TB)\n", CACHE_WRITE_SIZE_LIMIT, CACHE_WRITE_SIZE_LIMIT / (1024.0 * 1024.0 * 1024.0 * 1024.0));
    printf("remap_lba = %s\n", remap_lba ? "enabled (4K sequential allocation)" : "disabled");
    printf("cold_write_size_limit = %lu bytes (%.2f TB) %s\n", COLD_WRITE_SIZE_LIMIT, COLD_WRITE_SIZE_LIMIT / (1024.0 * 1024.0 * 1024.0 * 1024.0), COLD_WRITE_SIZE_LIMIT == 0 ? "[disabled]" : "");
    printf("loop_trace = %s\n", loop_trace ? "enabled" : "disabled");
    printf("min_trace_loops = %lld (cold_write_size_limit honored only after this many full passes)\n", min_trace_loops);
    printf("prefill = %s\n", no_fill ? "disabled" : "enabled");
    assert (cold_capacity > 0);
    // Factory 함수를 이용해 적절한 TraceParser 생성
    ITraceParser* parser = createTraceParser(trace_format);
    long max_cache_blocks = cache_size / block_size;
    printf("max_cache_blocks = %ld\n", max_cache_blocks);
    std::unique_ptr<ICache> cache(createCache(cache_policy, max_cache_blocks, cold_capacity, block_size, cache_trace, cache_trace_output, cold_trace_output, waf_log_file, valid_ratio, stat_log_file, periodic_ratio, util_step, moving_avg_type, moving_avg_window, gs_decision_period_segs));

    if (!no_fill) {
        std::cout << "[prefill] start: trace=" << trace_file
                  << ", limit=" << cold_capacity
                  << ", block_size=" << block_size << std::endl;
        // Prefill using trace until limit; uses same parser to avoid dup parsing logic differences.
        trace_prefill(*cache, trace_file, *parser, CACHE_WRITE_SIZE_LIMIT, block_size, cold_capacity);
    }

    // 통계 변수 초기화
    long long total_read = 0, total_write = 0;
    long long total_read_size = 0, total_write_size = 0;
    long long read_hit_size = 0, write_hit_size = 0;
    long long cache_write_size = 0, cold_tier_write_size = 0, cold_tier_read_size = 0;

    // --remap_lba: 4K-aligned trace LBA → sequential mapped LBA
    std::unordered_map<long long, long long> lba_remap;
    long long next_remap_lba = 0;
    auto issue_remapped = [&](long long off, long long sz, OP_TYPE op) {
        if (!remap_lba) {
            issue_op_to_cache(*cache, off, static_cast<int>(sz), op);
            return;
        }
        long long blk = block_size;
        long long end = off + sz;
        for (long long o = off; o < end; o += blk) {
            long long mapped;
            auto it = lba_remap.find(o);
            if (it == lba_remap.end()) {
                mapped = next_remap_lba;
                lba_remap.emplace(o, mapped);
                next_remap_lba += blk;
            } else {
                mapped = it->second;
            }
            issue_op_to_cache(*cache, mapped, static_cast<int>(blk), op);
        }
    };

    std::ifstream infile(trace_file);
    if (!infile) {
        std::cerr << "File Error" << std::endl;
        std::cerr << "Cannot open file: " << trace_file << std::endl;
        return 1;
    }
    
    std::string line;
    long long line_count = 0;
    const long long line_count_limit = 270000000000000000ULL;
    long long trace_loops = 0;

    while (line_count < line_count_limit) {
        if (!std::getline(infile, line)) {
            if (loop_trace) {
                trace_loops++;
                printf("[loop_trace] EOF reached, restarting trace (loop #%lld)\n", trace_loops);
                infile.clear();
                infile.seekg(0);
                if (!std::getline(infile, line)) {
                    break;  // empty file safeguard
                }
            } else {
                break;
            }
        }
        line_count++;
        if (line_count % 1000000 == 0) {
            print_stats(true, total_read, total_write, total_read_size, total_write_size, read_hit_size, write_hit_size, cache_write_size, cold_tier_write_size, cold_tier_read_size, max_cache_blocks, cache->size());
            if (remap_lba) {
                printf("[wss] next_remap_lba = %lld bytes (%.2f GB), unique_4k_blocks = %zu, trace_loops = %lld\n",
                       next_remap_lba, next_remap_lba / (1024.0*1024.0*1024.0), lba_remap.size(), trace_loops);
            }
        }
        cache->print_stats();
        if (static_cast<uint64_t>(cache_write_size) > CACHE_WRITE_SIZE_LIMIT) {
            break;
        }
        if (COLD_WRITE_SIZE_LIMIT > 0 && static_cast<uint64_t>(cold_tier_write_size) > COLD_WRITE_SIZE_LIMIT
            && trace_loops >= min_trace_loops) {
            break;
        }
        // 사용자 구현 parse_trace 함수 호출
        ParsedRow parsed = parser->parseTrace(line);
        // printf ("parsed.dev_id = %s, parsed.op_type = %s, parsed.lba_offset = %lld, parsed.lba_size = %d, parsed.timestamp = %f\n", parsed.dev_id.c_str(), parsed.op_type.c_str(), parsed.lba_offset, parsed.lba_size, parsed.timestamp);
        if (parsed.dev_id.empty()) {
            continue;
        }
        parsed.lba_offset *= lba_scale;
        parsed.lba_size   *= lba_scale;
        long long write_bytes_to_cache;
        long long evicted_blocks;
        if (parsed.op_type == "R" || parsed.op_type == "RS") {
            std::tie(write_bytes_to_cache, evicted_blocks, write_hit_size) = cache->get_status();
            //if (cache.is_cache_filled()) {
                total_read++;
                total_read_size += parsed.lba_size;
            //}
            if (policy == "all" || policy == "read-only") {
                issue_remapped(parsed.lba_offset, parsed.lba_size, OP_TYPE::READ);
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
                issue_remapped(parsed.lba_offset, parsed.lba_size, OP_TYPE::WRITE);
            }
            if (policy == "write-only") {
             //   cache->print_cache_trace(parsed.lba_offset, parsed.lba_size, OP_TYPE::WRITE);
            }

        }
    }
    
    double final_read_hit_ratio, final_write_hit_ratio;
    calc_hit_ratio(read_hit_size, total_read_size, write_hit_size, total_write_size, final_read_hit_ratio, final_write_hit_ratio);
    
    print_stats(false, total_read, total_write, total_read_size, total_write_size, read_hit_size, write_hit_size, cache_write_size, cold_tier_write_size, cold_tier_read_size, max_cache_blocks, cache->size());
    if (remap_lba) {
        printf("[wss] FINAL next_remap_lba = %lld bytes (%.2f GB), unique_4k_blocks = %zu, trace_loops = %lld\n",
               next_remap_lba, next_remap_lba / (1024.0*1024.0*1024.0), lba_remap.size(), trace_loops);
    }
    cache->print_stats();
    return 0;
}
