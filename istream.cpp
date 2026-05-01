#include "istream.h"
#include "sepbit.h"
#include "hot_cold.h"
#include "multi_hot_cold.h"
#include "hot_cold_midas.h"
#include "readwrite_stream.h"
#include "dogi_stream.h"
#include "nodap_stream.h"
#include <string>
#include <cassert>
#include <algorithm>

uint64_t interval = 1;
namespace {
constexpr int kMultiHotColdStreams = IStream::kDefaultGcStreams;
}

void set_stream_interval(uint64_t cache_block_count, uint64_t segment_size_blocks) {
    uint64_t computed = (uint64_t)(cache_block_count / (3));
    if (computed == 0) {
        computed = 1;
    }
    // Align interval to segment boundary if segment_size_blocks is specified
    if (segment_size_blocks > 0) {
        // Round up to nearest segment boundary
        computed = ((computed + segment_size_blocks - 1) / segment_size_blocks) * segment_size_blocks;
        if (computed == 0) {
            computed = segment_size_blocks;
        }
    }
    interval = computed;
}

IStream* createIstreamPolicy(std::string policy_type) {
    if (policy_type == "none" || policy_type.empty()) {
        return nullptr;
    }
    if (policy_type == "sepbit") {
        return new SepBIT();
    }
    else if (policy_type == "hotcold") {
        return new HotCold();
    } else if (policy_type == "multi_hotcold") {
        return new MultiHotCold(kMultiHotColdStreams, interval, false);
    }
    else if (policy_type == "multi_hotcold_create_timestamp_only") {
        return new MultiHotCold(kMultiHotColdStreams, interval, true);
    }
    else if (policy_type == "multi_hotcold_2") {
        return new MultiHotCold(kMultiHotColdStreams, interval, true, true, false);
    }
    else if (policy_type == "multi_hotcold_3") {
        return new MultiHotCold(kMultiHotColdStreams, interval, true, true, true);
    }
    else if (policy_type == "midas_hotcold") {
        return new MiDASHotCold();
    }
    else if (policy_type == "readwrite_hotcold") {
        // write hot/cold only, GC by created_timestamp
        return new ReadWriteStream(false, kMultiHotColdStreams, interval);
    }
    else if (policy_type == "readwrite_hotcold_read") {
        // write hot/cold + separate read GC stream
        return new ReadWriteStream(true, kMultiHotColdStreams, interval);
    }
    else if (policy_type == "readwrite_hotcold_circular") {
        // write hot/cold, circular cycle-based sub-streams
        return new ReadWriteStream(false, kMultiHotColdStreams, interval, true);
    }
    else if (policy_type == "readwrite_hotcold_read_circular") {
        // write hot/cold + read separation, circular cycle-based sub-streams
        return new ReadWriteStream(true, kMultiHotColdStreams, interval, true);
    }
    else if (policy_type == "dogi_heuristic") {
        return new DogiStream(/*num_gc_streams=*/6);
    }
    else if (policy_type == "nodap") {
        // NoDaP without oracle pre-pass: caller must invoke LoadOracle()
        // afterwards. Without it, every block is treated as cold (BIR=∞)
        // → all writes go to G_N. Used only for plumbing tests.
        return new NodapStream();
    }
    else {
        assert(false);
    }
}
