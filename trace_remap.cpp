#include "trace_parser.h"

#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

bool is_write(const std::string &op) {
    return (op == "W" || op == "WS");
}

long long align_down(long long v, long long unit) {
    return (v / unit) * unit;
}

// Map original block idx -> new block idx using modulo first, then fallback to free list.
class BlockRemapper {
public:
    explicit BlockRemapper(long long working_set_blocks)
        : used_(static_cast<std::size_t>(working_set_blocks), false) {
        free_list_.reserve(static_cast<std::size_t>(working_set_blocks));
        for (long long i = 0; i < working_set_blocks; ++i) {
            free_list_.push_back(i);
        }
    }

    long long map(long long orig_block, long long working_set_blocks) {
        auto it = mapping_.find(orig_block);
        if (it != mapping_.end()) return it->second;

        long long candidate = orig_block % working_set_blocks;
        long long mapped = -1;
        if (!used_[static_cast<std::size_t>(candidate)]) {
            mapped = candidate;
        } else {
            mapped = next_free();
            if (mapped == -1) {
                // exhausted: fall back to modulo even if reused
                mapped = candidate;
            }
        }
        used_[static_cast<std::size_t>(mapped)] = true;
        mapping_[orig_block] = mapped;
        return mapped;
    }

private:
    long long next_free() {
        while (!free_list_.empty()) {
            long long blk = free_list_.back();
            free_list_.pop_back();
            if (!used_[static_cast<std::size_t>(blk)]) return blk;
        }
        return -1;
    }

    std::unordered_map<long long, long long> mapping_;
    std::vector<char> used_;
    std::vector<long long> free_list_;
};

} // namespace

int main(int argc, char *argv[]) {
    if (argc < 5) {
        std::cerr << "Usage: " << argv[0]
                  << " --trace TRACE_FILE --write_tb N [--block_size BYTES] [--trace_format csv|blktrace] [--output OUT.csv]\n";
        return 1;
    }

    std::string trace_file;
    std::string trace_format = "csv";
    std::string output_file = "remapped_trace.csv";
    double write_tb = 0.0;
    long long write_bytes_limit = 0;
    int block_size = 4096;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--trace" && i + 1 < argc) {
            trace_file = argv[++i];
        } else if (arg == "--write_tb" && i + 1 < argc) {
            write_tb = std::stod(argv[++i]);
        } else if (arg == "--block_size" && i + 1 < argc) {
            block_size = std::stoi(argv[++i]);
        } else if (arg == "--trace_format" && i + 1 < argc) {
            trace_format = argv[++i];
        } else if (arg == "--output" && i + 1 < argc) {
            output_file = argv[++i];
        } else {
            std::cerr << "Unknown or incomplete argument: " << arg << "\n";
            return 1;
        }
    }

    if (trace_file.empty() || write_tb <= 0.0) {
        std::cerr << "trace file and write_tb are required\n";
        return 1;
    }
    // TB interpreted in binary (TiB).
    write_bytes_limit = static_cast<long long>(write_tb * 1024.0 * 1024.0 * 1024.0 * 1024.0);

    std::unique_ptr<ITraceParser> parser(createTraceParser(trace_format));
    if (!parser) {
        std::cerr << "Failed to create trace parser for format: " << trace_format << "\n";
        return 1;
    }

    // First pass: measure working set (unique blocks) until write_count is reached.
    std::ifstream in1(trace_file);
    if (!in1) {
        std::cerr << "Cannot open trace: " << trace_file << "\n";
        return 1;
    }

    std::unordered_set<long long> touched_blocks;
    long long bytes_seen = 0;
    std::string line;
    while (std::getline(in1, line) && bytes_seen < write_bytes_limit) {
        ParsedRow row = parser->parseTrace(line);
        if (row.dev_id.empty()) continue;
        if (!is_write(row.op_type)) continue;
        bytes_seen += row.lba_size;

        long long start_block = row.lba_offset / block_size;
        long long span_blocks = (row.lba_size + block_size - 1) / block_size;
        for (long long i = 0; i < span_blocks; ++i) {
            touched_blocks.insert(start_block + i);
        }
    }
    in1.close();

    if (touched_blocks.empty()) {
        std::cerr << "No writes observed to build working set\n";
        return 1;
    }

    long long working_set_blocks = static_cast<long long>(touched_blocks.size());
    long long working_set_bytes  = working_set_blocks * static_cast<long long>(block_size);

    std::cout << "[remap] bytes_seen=" << bytes_seen
              << ", working_set_blocks=" << working_set_blocks
              << ", working_set_bytes=" << working_set_bytes << "\n";

    BlockRemapper remapper(working_set_blocks);

    // Second pass: emit remapped trace (CSV) with LBA wrapped into working set.
    std::ifstream in2(trace_file);
    if (!in2) {
        std::cerr << "Cannot reopen trace: " << trace_file << "\n";
        return 1;
    }
    std::ofstream out(output_file);
    if (!out) {
        std::cerr << "Cannot open output file: " << output_file << "\n";
        return 1;
    }

    while (std::getline(in2, line)) {
        ParsedRow row = parser->parseTrace(line);
        if (row.dev_id.empty()) continue; // skip malformed

        long long start_block = row.lba_offset / block_size;
        long long span_blocks = (row.lba_size + block_size - 1) / block_size;
        std::vector<long long> mapped_blocks;
        mapped_blocks.reserve(static_cast<std::size_t>(span_blocks));
        for (long long i = 0; i < span_blocks; ++i) {
            long long mapped_blk = remapper.map(start_block + i, working_set_blocks);
            mapped_blocks.push_back(mapped_blk);
        }
        // merge consecutive mapped blocks for compact output
        long long run_start = mapped_blocks.front();
        long long run_len = 1;
        for (std::size_t i = 1; i < mapped_blocks.size(); ++i) {
            if (mapped_blocks[i] == mapped_blocks[i-1] + 1) {
                run_len++;
            } else {
                long long mapped_offset = run_start * static_cast<long long>(block_size);
                long long mapped_size   = run_len * static_cast<long long>(block_size);
                out << row.dev_id << ','
                    << row.op_type << ','
                    << mapped_offset << ','
                    << mapped_size << ','
                    << row.timestamp << '\n';
                run_start = mapped_blocks[i];
                run_len = 1;
            }
        }
        long long mapped_offset = run_start * static_cast<long long>(block_size);
        long long mapped_size   = run_len * static_cast<long long>(block_size);
        out << row.dev_id << ','
            << row.op_type << ','
            << mapped_offset << ','
            << mapped_size << ','
            << row.timestamp << '\n';
    }

    std::cout << "[remap] done -> " << output_file << "\n";
    return 0;
}
