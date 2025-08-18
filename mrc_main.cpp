#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <stdexcept>
#include <iomanip>

#include "trace_parser.h" // 제공된 파서 헤더
#include "mrc_calculator.h"

// --- 여기서부터는 제공된 파서의 모의(Mock) 구현입니다. ---
// 실제로는 이 부분을 컴파일 시 링크하게 됩니다.

void print_usage() {
    std::cerr << "Usage: ./mrc_analyzer --file <trace_file.csv> --type <LRU|OPT> [--H <num_buckets>]" << std::endl;
    std::cerr << "  --file : Path to the trace file." << std::endl;
    std::cerr << "  --type : Algorithm type (LRU or OPT)." << std::endl;
    std::cerr << "  --H    : Number of histogram buckets for MRC output (default: 1000)." << std::endl;
}

int main(int argc, char* argv[]) {
    std::string file_path;
    std::string algo_str;
    int num_buckets = 1000; // 기본값 설정
    uint64_t capacity = 0;

    // 1. 명시적 인자 파싱
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--file") {
            if (i + 1 < argc) file_path = argv[++i];
        } else if (arg == "--type") {
            if (i + 1 < argc) algo_str = argv[++i];
        } else if (arg == "--H") {
            if (i + 1 < argc) num_buckets = std::stoi(argv[++i]);
        } else if (arg == "--capacity") {
            if (i + 1 < argc) capacity = std::stoll(argv[++i]);
        }
    }
    assert (capacity > 0);

    if (file_path.empty() || algo_str.empty()) {
        std::cerr << "Error: --file and --type are required arguments." << std::endl;
        print_usage();
        return 1;
    }

    AlgorithmType algo_type;
    if (algo_str == "LRU") {
        algo_type = AlgorithmType::LRU;
    } else if (algo_str == "OPT") {
        algo_type = AlgorithmType::OPT;
    } else {
        std::cerr << "Error: Invalid algorithm type '" << algo_str << "'. Choose 'LRU' or 'OPT'." << std::endl;
        print_usage();
        return 1;
    }

    if (num_buckets <= 0) {
        std::cerr << "Error: Number of buckets (--H) must be positive." << std::endl;
        return 1;
    }

    std::ifstream trace_file(file_path);
    if (!trace_file.is_open()) {
        std::cerr << "Error: Could not open file " << file_path << std::endl;
        return 1;
    }

    // 2. 트레이스 파싱 및 블록화 (이전과 동일)
    constexpr long long BLOCK_SIZE = 4096; // 4KB
    std::vector<long long> trace;
    std::string line;

    std::cout << "Parsing and blockifying trace file..." << std::endl;
    ITraceParser* parser = createTraceParser("csv");
    uint64_t byte_limit = 4000000000000ULL; // 4TB
    uint64_t written_bytes = 0ULL;
    uint64_t next_print_written_bytes = 0ULL; // 100GB
    bool break_flag = false;
    while (std::getline(trace_file, line)) {
        ParsedRow parsed = parser->parseTrace(line);
        if (parsed.op_type == "W" || parsed.op_type == "WS") {
            if (parsed.lba_size <= 0) continue;

            long long start_block = parsed.lba_offset / BLOCK_SIZE;
            long long end_block = (parsed.lba_offset + parsed.lba_size - 1) / BLOCK_SIZE;
            
            for (long long block = start_block; block <= end_block; ++block) {
                trace.push_back(block);
                written_bytes += BLOCK_SIZE;
                if (written_bytes >= byte_limit) {
                    break_flag = true;
                }
            }
            // if wrriten_bytes exceeds every 100GB, print a message
            if (written_bytes >= next_print_written_bytes) {
                std::cout << "Parsing Written bytes: " << written_bytes / 1000000000
                            << " GB" << std::endl;
                next_print_written_bytes += 100000000000ULL; // 100GB
            }
        }
        if (break_flag) {
            std::cout << "Reached byte limit of 4TB, stopping trace parsing." << std::endl;
            break;
        }
    }
    delete parser;
    std::cout << "Total block accesses to analyze: " << trace.size() << std::endl;

    if (trace.empty()) {
        std::cout << "No valid 'W' or 'WS' operations found in the trace." << std::endl;
        return 0;
    }

    // 3. MRC 계산
    std::cout << "Calculating MRC for " << algo_str << " with " << num_buckets << " histogram buckets..." << std::endl;
    MrcCalculator calculator;
    std::map<long long, double> mrc = calculator.calculate_mrc(trace, capacity / BLOCK_SIZE, algo_type, num_buckets);

    // 4. 결과 출력
    std::cout << "\n--- Miss Rate Curve (MRC) ---\n";
    std::cout << "CacheSize(blocks),MissRate(%)\n";
    std::cout << std::fixed << std::setprecision(4);

    for (const auto& pair : mrc) {
        std::cout << pair.first << "," << pair.second * 100.0 << std::endl;
    }

    return 0;
}
