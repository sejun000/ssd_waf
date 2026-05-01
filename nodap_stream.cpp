#include "nodap_stream.h"
#include "trace_parser.h"
#include "cache_sim.h"
#include <algorithm>
#include <cassert>
#include <fstream>
#include <iostream>
#include <map>

uint64_t g_nodap_bir_upper[IStream::MAX_STREAMS] = {0};
int      g_nodap_num_groups = 0;

NodapStream::NodapStream(int num_groups, std::vector<uint64_t> bir_upper)
    : num_groups_(num_groups), bir_upper_(std::move(bir_upper))
{
    assert(static_cast<int>(bir_upper_.size()) == num_groups_);
    assert(num_groups_ <= IStream::MAX_STREAMS);
    // Publish to globals for score_nodap_expired.
    g_nodap_num_groups = num_groups_;
    for (int i = 0; i < num_groups_; ++i) {
        g_nodap_bir_upper[i] = bir_upper_[i];
    }
    std::cerr << "[NoDaP] constructor BIR table:";
    for (int i = 0; i < num_groups_; ++i) {
        if (bir_upper_[i] == UINT64_MAX) std::cerr << " inf";
        else std::cerr << " " << bir_upper_[i];
    }
    std::cerr << std::endl;
}

int NodapStream::classify_bir(uint64_t bir) const {
    for (int i = 0; i < num_groups_; ++i) {
        if (bir <= bir_upper_[i]) return i;
    }
    return num_groups_ - 1;
}

int NodapStream::Classify(uint64_t blockAddr, bool isGcAppend,
                          uint64_t global_timestamp,
                          uint64_t /*created_timestamp*/, bool /*is_read*/)
{
    ++classify_calls_;
    uint64_t inv_time;
    if (!isGcAppend) {
        auto it = next_inv_q_.find(blockAddr);
        if (it != next_inv_q_.end() && !it->second.empty()) {
            inv_time = it->second.front();
            it->second.pop_front();
            if (it->second.empty()) {
                next_inv_q_.erase(it);
            }
        } else {
            inv_time = UINT64_MAX;
            ++classify_misses_;
        }
        current_inv_[blockAddr] = inv_time;
    } else {
        auto it = current_inv_.find(blockAddr);
        inv_time = (it != current_inv_.end()) ? it->second : UINT64_MAX;
    }

    uint64_t bir = (inv_time > global_timestamp) ? (inv_time - global_timestamp)
                                                 : 0;
    return classify_bir(bir);
}

void NodapStream::LoadOracle(const std::string& trace_file,
                             ITraceParser& parser,
                             int block_size, int lba_scale)
{
    std::ifstream infile(trace_file);
    if (!infile) {
        std::cerr << "[NoDaP] cannot open trace: " << trace_file << std::endl;
        return;
    }

    uint64_t synthetic_ts = 0;
    uint64_t total_writes_lines = 0;
    uint64_t total_reads_lines  = 0;
    uint64_t next_log = 100ULL * 1000 * 1000;
    std::string line;

    while (std::getline(infile, line)) {
        ParsedRow parsed = parser.parseTrace(line);
        if (parsed.dev_id.empty()) continue;
        parsed.lba_offset *= lba_scale;
        parsed.lba_size   *= lba_scale;

        if (parsed.op_type == "R" || parsed.op_type == "RS") {
            ++total_reads_lines;
            continue;
        }
        if (parsed.op_type != "W" && parsed.op_type != "WS") continue;
        ++total_writes_lines;

        // Replicate issue_op_to_cache / batch_insert block decomposition.
        long start_block = static_cast<long>(parsed.lba_offset / block_size);
        long end_block   = static_cast<long>((parsed.lba_offset + parsed.lba_size) / block_size);
        long long req_start = parsed.lba_offset;
        long long req_end   = parsed.lba_offset + parsed.lba_size;

        std::map<long, int> newBlocks;
        for (long block = start_block; block <= end_block; ++block) {
            long long block_start = static_cast<long long>(block) * block_size;
            long long block_end   = block_start + block_size;
            long long left  = std::max(block_start, req_start);
            long long right = std::min(block_end,   req_end);
            if (right <= left) continue;
            newBlocks[block] = static_cast<int>(right - left);
        }

        for (auto& kv : newBlocks) {
            uint64_t lba = static_cast<uint64_t>(kv.first);
            auto& q = next_inv_q_[lba];
            if (!q.empty()) {
                // Latest pending entry (= UINT64_MAX) is this LBA's previous
                // write; its invalidation occurs NOW (= synthetic_ts).
                q.back() = synthetic_ts;
            }
            q.push_back(UINT64_MAX);
            ++synthetic_ts;
        }

        if (synthetic_ts >= next_log) {
            std::cout << "[NoDaP] pre-pass: " << (synthetic_ts / 1000000)
                      << "M block-write events, "
                      << "lba_table=" << next_inv_q_.size() << std::endl;
            next_log += 100ULL * 1000 * 1000;
        }
    }

    oracle_loaded_ = true;
    std::cout << "[NoDaP] oracle loaded: " << synthetic_ts
              << " block-write events from " << total_writes_lines
              << " trace writes (" << total_reads_lines
              << " reads skipped), unique LBAs=" << next_inv_q_.size()
              << std::endl;
}
