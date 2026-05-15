#include "istream.h"
#include "sepbit.h"
#include "hot_cold.h"
#include "multi_hot_cold.h"
#include "hot_cold_midas.h"
#include <string>
#include <cassert>
#include <algorithm>

extern uint64_t g_threshold;  // from icache.cpp; LogCache updates it every GC tick.

uint64_t interval = 1;
namespace {
constexpr int kMultiHotColdStreams = 5;
uint64_t g_stream_segment_size_blocks = 0;   // saved by set_stream_interval, used for align
uint64_t g_stream_fallback_blocks     = 0;   // cache_block_count, used when g_threshold == 0
}

uint64_t compute_stream_interval(uint64_t fallback_cache_blocks) {
    uint64_t base = (g_threshold > 0) ? g_threshold
                  : (fallback_cache_blocks > 0 ? fallback_cache_blocks
                                                : g_stream_fallback_blocks);
    if (base == 0) return interval;  // nothing to work with → keep prior value
    uint64_t computed = base / kMultiHotColdStreams;
    if (computed == 0) computed = 1;
    if (g_stream_segment_size_blocks > 0) {
        uint64_t seg = g_stream_segment_size_blocks;
        computed = ((computed + seg - 1) / seg) * seg;
        if (computed == 0) computed = seg;
    }
    return computed;
}

void set_stream_interval(uint64_t cache_block_count, uint64_t segment_size_blocks) {
    g_stream_segment_size_blocks = segment_size_blocks;
    g_stream_fallback_blocks     = cache_block_count;
    interval = compute_stream_interval(cache_block_count);
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
    else {
        assert(false);
    }
}
