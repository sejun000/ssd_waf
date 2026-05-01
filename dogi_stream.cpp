#include "dogi_stream.h"
#include "icache.h"  // g_dogi_hot_bir
#include "dogi/app/global.h"
#include <cstdio>
#include <algorithm>

namespace {
std::size_t dogi_bucket_index(uint64_t v) {
    if (v <= 7) return static_cast<std::size_t>(v);
    if (v <= 15) return 8;
    if (v <= 31) return 9;
    if (v <= 63) return 10;
    return 11;
}
}

DogiStream::DogiStream(int num_gc_streams, uint64_t hot_threshold_init,
                       uint64_t hot_step, uint64_t hot_window_blocks,
                       uint64_t frozen_clear_interval)
    : num_gc_streams_(num_gc_streams),
      hot_threshold_(hot_threshold_init),
      hot_step_(hot_step),
      hot_window_blocks_(hot_window_blocks),
      frozen_clear_interval_(frozen_clear_interval)
{
    next_frozen_clear_ = frozen_clear_interval;
}

uint8_t DogiStream::estimateInterval(uint64_t blockAddr, uint64_t segmentAge) {
    if (!tracksHotState(blockAddr)) {
        return 255;
    }
    auto it = intervals_.find(blockAddr);
    uint8_t base = (it != intervals_.end()) ? it->second : 0;
    uint64_t delta = segmentAge / kIntervalUnit;
    return saturatingAdd(base, delta);
}

void DogiStream::adjustHotThreshold(double current_waf) {
    // Paper §4.2.1: if WAF decreased, keep going same direction (+d);
    // if increased, reverse (-d). If within 5%, hold steady.
    if (prev_window_waf_ == DBL_MAX) {
        prev_window_waf_ = current_waf;
        return;
    }
    double diff = (current_waf - prev_window_waf_) / prev_window_waf_;
    // μ adjustment disabled: match DOGI APPLY_ML=0 (fixed threshold)
    // if (diff < -0.05) hot_threshold_ += hot_step_;
    // else if (diff > 0.05 && hot_threshold_ > hot_step_) hot_threshold_ -= hot_step_;
    // g_dogi_hot_bir은 icache.cpp에서 196608 (= 2*kSegmentBlocks) 고정. 덮어쓰지 않음.
    prev_window_waf_ = current_waf;
}

int DogiStream::Classify(uint64_t blockAddr, bool isGcAppend,
                         uint64_t global_timestamp,
                         uint64_t created_timestamp, bool is_read) {
    if (!isGcAppend) {
        // ── Host write: Hot Filter (8-bit counter, matching DOGI original) ──
        // Warmup: before pass_time_blocks_, all blocks are non-hot (like DOGI main.cc)
        uint64_t segmentAge = (created_timestamp < global_timestamp)
            ? (global_timestamp - created_timestamp) : 0;
        uint8_t est = estimateInterval(blockAddr, segmentAge);
        const uint64_t age_units = segmentAge / kIntervalUnit;
        ++g_dogi_host_age_bucket_counts[dogi_bucket_index(age_units)];
        ++g_dogi_host_est_bucket_counts[dogi_bucket_index(est)];
        if (tracksHotState(blockAddr)) {
            intervals_[blockAddr] = 0;
        }
        bool isHot = (total_writes_ > pass_time_blocks_)
            ? (static_cast<uint64_t>(est) < hot_threshold_)
            : false;
        if (isHot) ++g_dogi_host_hot_writes;
        else ++g_dogi_host_cold_writes;
        // Track the block's current logical DOGI group so first GC relocation
        // can start from host group 0/1 like standalone NO_ML.
        gc_stream_[blockAddr] = isHot ? 0 : 1;
        return isHot ? 0 : 1;
    }

    // ── GC append ──

    // Accumulate segment age into 8-bit counter (like DOGI AccumulateGcAge)
    uint64_t segmentAge = (created_timestamp < global_timestamp)
        ? (global_timestamp - created_timestamp) : 0;
    if (tracksHotState(blockAddr)) {
        uint64_t delta = segmentAge / kIntervalUnit;
        auto it2 = intervals_.find(blockAddr);
        uint8_t base = (it2 != intervals_.end()) ? it2->second : 0;
        intervals_[blockAddr] = saturatingAdd(base, delta);
    }

    // Frozen Filter: never overwritten → frozen stream (coldest)
    auto it = ever_overwritten_.find(blockAddr);
    if (it == ever_overwritten_.end() || !it->second) {
        ++g_dogi_gc_frozen_writes;
        return num_gc_streams_ - 1; // standalone NO_ML frozen group = logical last group
    }
    ++g_dogi_gc_nonfrozen_writes;

    // Age-based cascade: look up previous GC stream, move one colder
    auto gc_it = gc_stream_.find(blockAddr);
    int prev_stream = 0;
    if (gc_it != gc_stream_.end()) {
        prev_stream = gc_it->second;
    }

    int next_stream = prev_stream + 1;
    int max_non_frozen = num_gc_streams_ - 2;
    if (next_stream > max_non_frozen) {
        next_stream = max_non_frozen;
    }

    gc_stream_[blockAddr] = next_stream;
    return next_stream; // standalone NO_ML reuses the same logical group ids for host/GC
}

void DogiStream::Append(uint64_t blockAddr, uint64_t global_timestamp, void *arg) {
    ++total_writes_;

    // At warmup boundary, mark every block touched during warmup as
    // ever-overwritten. Stands in for SSD pre-conditioning so --no_fill
    // doesn't leave residual warmup blocks as permanent frozen (u=1.0
    // class-5 self-loop in score_dogi_expired_first GC).
    if (total_writes_ == pass_time_blocks_ + 1) {
        ever_overwritten_.reserve(intervals_.size());
        for (const auto &kv : intervals_) {
            ever_overwritten_[kv.first] = true;
        }
    }

    // Track overwrites for Frozen Filter (only after warmup, like DOGI main.cc)
    if (total_writes_ > pass_time_blocks_) {
        auto it = intervals_.find(blockAddr);
        if (it != intervals_.end()) {
            ever_overwritten_[blockAddr] = true;
        }
    }

    // User write counter for Hot Filter WAF tracking
    ++window_user_writes_;

    // Frozen Filter clock clear disabled: DOGI standalone has no clear mechanism.
    // if (global_timestamp >= next_frozen_clear_) {
    //     ever_overwritten_.clear();
    //     next_frozen_clear_ = global_timestamp + frozen_clear_interval_;
    // }

    // Hot Filter threshold adjustment at window boundary
    if (window_user_writes_ >= hot_window_blocks_) {
        double waf = (window_user_writes_ + window_gc_writes_ > 0)
            ? static_cast<double>(window_user_writes_ + window_gc_writes_) / window_user_writes_
            : 1.0;
        adjustHotThreshold(waf);
        window_user_writes_ = 0;
        window_gc_writes_ = 0;
    }
}

void DogiStream::GcAppend(uint64_t blockAddr) {
    ++window_gc_writes_;
}

void DogiStream::CollectSegment(Segment *segment, uint64_t global_timestamp) {
    // No special collection logic needed
}

int DogiStream::GetVictimStreamId(uint64_t global_timestamp, uint64_t threshold) {
    // No expired segment tracking in heuristic mode
    return -1;
}
