#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <unordered_map>
#include <vector>

// Segment-granular flush ghost cache.
//
//   capacity = D segments (= gs_decision_period_segs_).
//
// Semantics:
//   - pushSegment(blocks): a whole segment was flushed by log cache. Insert
//     it at the back; if size now exceeds D, drop oldest segment entirely
//     (its blocks are removed from the lookup, its valid_count is forgotten).
//   - invalidate(block_id): a host write hit a block currently in ghost.
//     Decrement that segment's valid_count and remove the lookup entry, so
//     subsequent invalidate(b) calls on the same key are no-ops.
//   - totalValidCount(): Σ valid_count across the D resident segments.
//
// "Same block flushed twice" case: only the most recent containing segment
// is treated as still-valid. When pushSegment re-encounters a block that
// already maps to an older segment, the older segment's valid_count is
// decremented and the lookup is re-pointed to the new segment.
class AgeGhostCache {
public:
    explicit AgeGhostCache(std::size_t capacity_segs);

    void pushSegment(const std::vector<uint64_t>& blocks);
    bool invalidate(uint64_t block_id);

    uint64_t    totalValidCount() const { return total_valid_count_; }
    // Σ valid_count_at_push_time across D resident segs (never decremented
    // by invalidate; only changes when a whole segment ages out).
    uint64_t    totalInitialValidCount() const { return total_initial_count_; }
    // Cumulative valid pages pushed into ghost (= total ever flushed).  Never
    // decremented.  EWMA(rate) per host write ≈ flush valid rate.
    uint64_t    totalPushValidCount() const { return total_push_valid_count_; }
    // Cumulative valid pages popped out of ghost as front-of-FIFO eviction
    // (= still flushed even with D-seg extension).  Per-host-write EWMA
    // gives the flush rate after applying the extension.  F_push − F_pop
    // ≈ saved-by-extension rate per host write.
    uint64_t    totalPopValidCount() const { return total_pop_valid_count_; }
    std::size_t segCount() const { return segs_.size(); }

    void        setCapacity(std::size_t capacity_segs);
    std::size_t capacity() const { return capacity_segs_; }

    void reset();

private:
    struct GhostSeg {
        uint64_t valid_count;
        std::vector<uint64_t> blocks;
    };
    using SegIt = std::list<GhostSeg>::iterator;

    void evict_oldest();

    std::size_t capacity_segs_;
    std::list<GhostSeg> segs_;
    std::unordered_map<uint64_t, SegIt> block_to_seg_;
    uint64_t total_valid_count_;
    uint64_t total_initial_count_;
    uint64_t total_push_valid_count_;  // cumulative valid pages pushed in
    uint64_t total_pop_valid_count_;   // cumulative valid pages popped (aged out)
};
