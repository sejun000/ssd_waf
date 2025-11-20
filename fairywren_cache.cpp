#include "fairywren_cache.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace {
constexpr double HOT_CRITICAL_RATIO   = 0.015; // 1.5%
constexpr double HOT_RELAX_RATIO      = 0.05;  // 5%
constexpr double COLD_CRITICAL_RATIO  = 0.015; // 1.5%
constexpr double FWLOG_CRITICAL_RATIO = 0.15;  // 15%

void fw_assert(bool cond, const char* msg) {
    if (!cond) {
        std::fprintf(stderr, "[FairyWren] %s\n", msg);
        assert(cond && "[FairyWren] invariant violated");
    }
}
}

extern uint64_t interval;

FairyWrenCache::FairyWrenCache(uint64_t           cold_capacity,
                               uint64_t           cache_block_count,
                               int                cache_block_size,
                               bool               cache_trace,
                               const std::string& trace_file,
                               const std::string& cold_trace_file,
                               std::string&       waf_log_file,
                               const FairyWrenConfig& cfg,
                               std::string        stat_log_file)
    : ICache(cold_capacity, waf_log_file, stat_log_file),
      cache_block_size_(cache_block_size),
      cfg_(cfg),
      evicted_ages_histogram_(std::make_unique<Histogram>("fw_evicted_ages", interval, HISTOGRAM_BUCKETS * 2, fp_stats)),
      evicted_blocks_histogram_(std::make_unique<Histogram>("fw_evicted_blocks", 1, HISTOGRAM_BUCKETS, fp_stats)),
      migrated_blocks_histogram_(std::make_unique<Histogram>("fw_migrated_blocks", 1, HISTOGRAM_BUCKETS, fp_stats)),
      evicted_ages_with_segment_histogram_(std::make_unique<Histogram>("fw_evicted_segment_age", interval, HISTOGRAM_BUCKETS * 2, fp_stats)),
      migrated_ages_with_segment_histogram_(std::make_unique<Histogram>("fw_migrated_segment_age", interval, HISTOGRAM_BUCKETS * 2, fp_stats)),
      migrated_ages_histogram_(std::make_unique<Histogram>("fw_migrated_ages", interval, HISTOGRAM_BUCKETS * 2, fp_stats)) {
    (void)cache_trace;
    (void)trace_file;
    (void)cold_trace_file;
    segment_size_blocks_ = cfg_.segment_bytes / cache_block_size_;
    if (segment_size_blocks_ == 0) {
        throw std::runtime_error("FairyWrenCache: invalid segment size");
    }
    total_segments_ = cache_block_count * cache_block_size_ / cfg_.segment_bytes;
    if (total_segments_ == 0) {
        total_segments_ = 1;
    }
    total_cache_block_count_ = cache_block_count;
    total_capacity_bytes_    = cache_block_count * cache_block_size_;
    next_stats_print_bytes_  = cfg_.segment_bytes;

    regions_[region_index(RegionKind::FWLOG)].name = "fwlog";
    regions_[region_index(RegionKind::HOT)].name   = "hot";
    regions_[region_index(RegionKind::COLD)].name  = "cold";

    initialize_regions(cache_block_count);
}

FairyWrenCache::~FairyWrenCache() = default;

bool FairyWrenCache::exists(long key) {
    return mapping_.find(key) != mapping_.end();
}

void FairyWrenCache::batch_insert(int /*stream_id*/,
                                  const std::map<long, int>& newBlocks,
                                  OP_TYPE op_type) {
    if (op_type == OP_TYPE::READ || newBlocks.empty()) {
        return;
    }

    for (auto [key, lba_sz] : newBlocks) {
        append_host_block(key, lba_sz);
        maybe_run_gc();
    }
}

bool FairyWrenCache::is_cache_filled() {
    return region(RegionKind::FWLOG).free_segments.empty();
}

void FairyWrenCache::evict_one_block() {
    // Segment-level GC handles reclamation.
}

void FairyWrenCache::initialize_regions(uint64_t /*cache_block_count*/) {
    std::size_t fwlog_segments =
        std::max<std::size_t>(1, static_cast<std::size_t>(
                                      std::round(total_segments_ * cfg_.fwlog_ratio)));
    fwlog_segments = std::min(fwlog_segments, total_segments_);

    std::size_t remaining = total_segments_ - fwlog_segments;
    std::size_t hot_segments = remaining;
    if (remaining > 0) {
        hot_segments = std::max<std::size_t>(
            1,
            static_cast<std::size_t>(std::round(remaining * cfg_.hot_ratio_of_remaining)));
        hot_segments = std::min(hot_segments, remaining);
    }
    std::size_t cold_segments = total_segments_ - fwlog_segments - hot_segments;
    if (cold_segments == 0 && total_segments_ > 1) {
        if (hot_segments > 1) {
            --hot_segments;
            ++cold_segments;
        } else if (fwlog_segments > 1) {
            --fwlog_segments;
            ++cold_segments;
        }
    }

    regions_[region_index(RegionKind::FWLOG)].total_segments = fwlog_segments;
    regions_[region_index(RegionKind::HOT)].total_segments   = hot_segments;
    regions_[region_index(RegionKind::COLD)].total_segments  = cold_segments;
    fw_assert(fwlog_segments + hot_segments + cold_segments == total_segments_,
              "segment partition mismatch");

    for (std::size_t i = 0; i < total_segments_; ++i) {
        all_segments_.push_back(
            std::make_unique<LogCacheSegment>(segment_size_blocks_, log_cache_timestamp_));
    }

    auto it = all_segments_.begin();
    for (std::size_t i = 0; i < fwlog_segments && it != all_segments_.end(); ++i, ++it) {
        (*it)->reset();
        regions_[region_index(RegionKind::FWLOG)].free_segments.push_back(it->get());
        segment_region_[it->get()] = RegionKind::FWLOG;
    }
    for (std::size_t i = 0; i < hot_segments && it != all_segments_.end(); ++i, ++it) {
        (*it)->reset();
        regions_[region_index(RegionKind::HOT)].free_segments.push_back(it->get());
        segment_region_[it->get()] = RegionKind::HOT;
    }
    for (; it != all_segments_.end(); ++it) {
        (*it)->reset();
        regions_[region_index(RegionKind::COLD)].free_segments.push_back(it->get());
        segment_region_[it->get()] = RegionKind::COLD;
    }
}

FairyWrenCache::RegionState& FairyWrenCache::region(RegionKind kind) {
    return regions_[region_index(kind)];
}

const FairyWrenCache::RegionState& FairyWrenCache::region(RegionKind kind) const {
    return regions_[region_index(kind)];
}

std::size_t FairyWrenCache::region_index(RegionKind kind) const {
    return static_cast<std::size_t>(kind);
}

LogCacheSegment* FairyWrenCache::ensure_active_segment(RegionKind kind, bool allow_force) {
    RegionState& reg = region(kind);
    if (reg.active && reg.active->full()) {
        push_to_fifo(kind, reg.active);
        reg.active = nullptr;
    }

    if (!reg.active) {
        LogCacheSegment* seg = take_free_segment(kind);
        if (!seg && allow_force) {
            if (force_reclaim(kind)) {
                seg = take_free_segment(kind);
            }
        }
        if (!seg) {
            return nullptr;
        }
        seg->reset();
        seg->create_timestamp = log_cache_timestamp_;
        reg.active = seg;
        move_segment_region(seg, kind);
    }
    return reg.active;
}

void FairyWrenCache::close_active_segment(RegionKind kind) {
    RegionState& reg = region(kind);
    if (!reg.active) {
        return;
    }
    push_to_fifo(kind, reg.active);
    reg.active = nullptr;
}

void FairyWrenCache::push_to_fifo(RegionKind kind, LogCacheSegment* seg) {
    if (!seg) {
        return;
    }
    if (seg->valid_cnt == 0) {
        release_segment_to_free(kind, seg);
        return;
    }
    region(kind).fifo_segments.push_back(seg);
}

LogCacheSegment* FairyWrenCache::pop_victim(RegionKind kind) {
    RegionState& reg = region(kind);
    if (reg.fifo_segments.empty() && reg.active && reg.active->valid_cnt > 0) {
        push_to_fifo(kind, reg.active);
        reg.active = nullptr;
    }
    if (reg.fifo_segments.empty()) {
        return nullptr;
    }
    LogCacheSegment* seg = reg.fifo_segments.front();
    reg.fifo_segments.pop_front();
    return seg;
}

LogCacheSegment* FairyWrenCache::take_free_segment(RegionKind kind) {
    RegionState& reg = region(kind);
    if (reg.free_segments.empty()) {
        return nullptr;
    }
    LogCacheSegment* seg = reg.free_segments.front();
    reg.free_segments.pop_front();
    return seg;
}

void FairyWrenCache::release_segment_to_free(RegionKind kind, LogCacheSegment* seg) {
    if (!seg) {
        return;
    }
    fw_assert(seg->blocks.size() == segment_size_blocks_,
              "segment size mismatch when releasing to free list");
    fw_assert(seg->valid_cnt == 0, "releasing segment with valid blocks");
    seg->reset();
    region(kind).free_segments.push_back(seg);
    move_segment_region(seg, kind);
}

bool FairyWrenCache::steal_free_segment(RegionKind donor, RegionKind receiver) {
    LogCacheSegment* seg = take_free_segment(donor);
    if (!seg) {
        return false;
    }
    seg->reset();
    region(receiver).free_segments.push_back(seg);
    move_segment_region(seg, receiver);
    return true;
}

void FairyWrenCache::move_segment_region(LogCacheSegment* seg, RegionKind new_kind) {
    auto it = segment_region_.find(seg);
    if (it == segment_region_.end()) {
        segment_region_[seg] = new_kind;
        return;
    }
    RegionKind old_kind = it->second;
    if (old_kind != new_kind) {
        adjust_region_valid(old_kind, -static_cast<int64_t>(seg->valid_cnt));
        adjust_region_valid(new_kind, static_cast<int64_t>(seg->valid_cnt));
    }
    it->second = new_kind;
}

FairyWrenCache::RegionKind FairyWrenCache::current_region(LogCacheSegment* seg) const {
    auto it = segment_region_.find(seg);
    if (it == segment_region_.end()) {
        fw_assert(false, "segment missing region assignment");
        return RegionKind::FWLOG;
    }
    return it->second;
}

void FairyWrenCache::adjust_region_valid(RegionKind kind, int64_t delta) {
    auto idx = region_index(kind);
    int64_t new_value = static_cast<int64_t>(region_valid_blocks_[idx]) + delta;
    fw_assert(new_value >= 0, "region valid block count underflow");
    region_valid_blocks_[idx] = static_cast<uint64_t>(new_value);
}

void FairyWrenCache::adjust_region_valid(LogCacheSegment* seg, int64_t delta) {
    adjust_region_valid(current_region(seg), delta);
}

void FairyWrenCache::append_host_block(long key, int lba_sz) {
    LogCacheSegment* seg = ensure_active_segment(RegionKind::FWLOG);
    if (!seg) {
        throw std::runtime_error("FairyWrenCache: no free FWLOG segment");
    }
    invalidate(key, lba_sz);
    append_block(seg, key, log_cache_timestamp_);
    ++log_cache_timestamp_;
    write_size_to_cache += lba_sz;
}

void FairyWrenCache::append_block(LogCacheSegment* seg, long key, uint64_t timestamp) {
    if (!seg) {
        return;
    }
    fw_assert(seg->write_ptr < seg->blocks.size(), "write pointer exceeds segment bounds");
    seg->blocks[seg->write_ptr] = { key, true, timestamp };
    mapping_[key]               = { seg, seg->write_ptr };
    ++seg->write_ptr;
    ++seg->valid_cnt;
    adjust_region_valid(seg, 1);
}

bool FairyWrenCache::migrate_block(LogCacheSegment* src,
                                   std::size_t      idx,
                                   RegionKind       dest) {
    auto& blk = src->blocks[idx];
    if (!blk.valid) {
        return true;
    }
    RegionKind src_kind = current_region(src);
    auto it = mapping_.find(blk.key);
    if (it == mapping_.end() || it->second.seg != src || it->second.idx != idx) {
        blk.valid = false;
        if (src->valid_cnt > 0) {
            --src->valid_cnt;
        }
        adjust_region_valid(src_kind, -1);
        return true;
    }
    uint64_t block_age = log_cache_timestamp_ - blk.create_timestamp;
    bool allow_force = dest != current_region(src);
    LogCacheSegment* dest_seg = ensure_active_segment(dest, allow_force);
    if (!dest_seg) {
        return false;
    }
    RegionKind dest_kind = current_region(dest_seg);
    auto& dst_blk = dest_seg->blocks[dest_seg->write_ptr];
    dst_blk.key              = blk.key;
    dst_blk.valid            = true;
    dst_blk.create_timestamp = blk.create_timestamp;

    it->second = { dest_seg, dest_seg->write_ptr };
    ++dest_seg->write_ptr;
    ++dest_seg->valid_cnt;
    adjust_region_valid(dest_kind, 1);
    ++migrated_blocks_;
    if (migrated_ages_histogram_) {
        migrated_ages_histogram_->inc(block_age);
    }

    blk.valid = false;
    if (src->valid_cnt > 0) {
        --src->valid_cnt;
    }
    adjust_region_valid(src_kind, -1);
    return true;
}

bool FairyWrenCache::copy_valid_blocks(LogCacheSegment* src, RegionKind dest, uint64_t* moved_blocks) {
    if (!src) {
        if (moved_blocks) {
            *moved_blocks = 0;
        }
        return false;
    }
    bool moved_all = true;
    uint64_t moved = 0;
    for (std::size_t i = 0; i < src->blocks.size(); ++i) {
        if (src->blocks[i].valid) {
            if (!migrate_block(src, i, dest)) {
                moved_all = false;
                break;
            }
            ++moved;
        }
    }
    if (moved_blocks) {
        *moved_blocks = moved;
    }
    if (moved > 0) {
        if (migrated_blocks_histogram_) {
            migrated_blocks_histogram_->inc(moved);
        }
        if (migrated_ages_with_segment_histogram_) {
            migrated_ages_with_segment_histogram_->inc(log_cache_timestamp_ - src->create_timestamp);
        }
    }
    return moved_all && src->valid_cnt == 0;
}

void FairyWrenCache::drop_segment(LogCacheSegment* seg) {
    if (!seg) {
        return;
    }
    RegionKind seg_kind = current_region(seg);
    adjust_region_valid(seg_kind, -static_cast<int64_t>(seg->valid_cnt));
    uint64_t evicted_blocks_for_segment = 0;
    for (auto& blk : seg->blocks) {
        if (!blk.valid) {
            continue;
        }
        mapping_.erase(blk.key);
        blk.valid = false;
        if (evicted_ages_histogram_) {
            evicted_ages_histogram_->inc(log_cache_timestamp_ - blk.create_timestamp);
        }
        _evict_one_block(blk.key * cache_block_size_, cache_block_size_, OP_TYPE::WRITE);
        evicted_blocks += 1;
        ++evicted_blocks_for_segment;
    }
    if (evicted_blocks_for_segment > 0) {
        if (evicted_blocks_histogram_) {
            evicted_blocks_histogram_->inc(evicted_blocks_for_segment);
        }
        if (evicted_ages_with_segment_histogram_) {
            evicted_ages_with_segment_histogram_->inc(log_cache_timestamp_ - seg->create_timestamp);
        }
    }
    seg->valid_cnt = 0;
    seg->write_ptr = 0;
}

void FairyWrenCache::invalidate(long key, int lba_sz) {
    auto it = mapping_.find(key);
    if (it == mapping_.end()) {
        _invalidate_cold_block(key * cache_block_size_, lba_sz, OP_TYPE::TRIM);
        return;
    }
    auto& loc = it->second;
    if (loc.seg && loc.idx < loc.seg->blocks.size() &&
        loc.seg->blocks[loc.idx].valid) {
        loc.seg->blocks[loc.idx].valid = false;
        if (loc.seg->valid_cnt > 0) {
            --loc.seg->valid_cnt;
        }
        adjust_region_valid(loc.seg, -1);
        ++invalidate_blocks_;
    }
    mapping_.erase(it);
}

void FairyWrenCache::maybe_run_gc() {
    if (needs_gc(RegionKind::HOT, HOT_CRITICAL_RATIO)) {
        if (reclaim_from_hot()) {
            return;
        }
    }
    if (needs_gc(RegionKind::COLD, COLD_CRITICAL_RATIO)) {
        if (reclaim_from_cold()) {
            return;
        }
    }
    if (needs_gc(RegionKind::FWLOG, FWLOG_CRITICAL_RATIO)) {
        if (reclaim_from_fwlog()) {
            return;
        }
    }
    if (needs_gc(RegionKind::HOT, HOT_RELAX_RATIO)) {
        reclaim_from_hot();
    }
}

bool FairyWrenCache::needs_gc(RegionKind kind, double ratio) const {
    const RegionState& reg = region(kind);
    if (reg.total_segments == 0) {
        return false;
    }
    std::size_t threshold =
        static_cast<std::size_t>(std::ceil(reg.total_segments * ratio));
    threshold = std::max<std::size_t>(threshold, 1);
    return reg.free_segments.size() <= threshold;
}

bool FairyWrenCache::reclaim_from_fwlog() {
    LogCacheSegment* victim = pop_victim(RegionKind::FWLOG);
    if (!victim) {
        return false;
    }

    if (victim->valid_cnt == 0) {
        release_segment_to_free(RegionKind::FWLOG, victim);
        return true;
    }

    if (victim->valid_cnt == segment_size_blocks_) {
        if (steal_free_segment(RegionKind::HOT, RegionKind::FWLOG)) {
            move_segment_region(victim, RegionKind::HOT);
            region(RegionKind::HOT).fifo_segments.push_back(victim);
            return true;
        }
    }

    if (!copy_valid_blocks(victim, RegionKind::HOT)) {
        region(RegionKind::FWLOG).fifo_segments.push_front(victim);
        return false;
    }
    release_segment_to_free(RegionKind::FWLOG, victim);
    return true;
}

bool FairyWrenCache::reclaim_from_hot() {
    LogCacheSegment* victim = pop_victim(RegionKind::HOT);
    if (!victim) {
        return false;
    }

    if (victim->valid_cnt == 0) {
        release_segment_to_free(RegionKind::HOT, victim);
        return true;
    }

    if (victim->valid_cnt == segment_size_blocks_) {
        if (steal_free_segment(RegionKind::COLD, RegionKind::HOT)) {
            move_segment_region(victim, RegionKind::COLD);
            region(RegionKind::COLD).fifo_segments.push_back(victim);
            return true;
        }
    }

    if (!copy_valid_blocks(victim, RegionKind::COLD)) {
        region(RegionKind::HOT).fifo_segments.push_front(victim);
        return false;
    }
    release_segment_to_free(RegionKind::HOT, victim);
    return true;
}

bool FairyWrenCache::reclaim_from_cold() {
    LogCacheSegment* victim = pop_victim(RegionKind::COLD);
    if (!victim) {
        return false;
    }

    if (victim->valid_cnt == 0) {
        release_segment_to_free(RegionKind::COLD, victim);
        return true;
    }

    if (victim->valid_cnt == segment_size_blocks_) {
        drop_segment(victim);
        release_segment_to_free(RegionKind::COLD, victim);
        return true;
    }

    if (!copy_valid_blocks(victim, RegionKind::COLD)) {
        region(RegionKind::COLD).fifo_segments.push_front(victim);
        return false;
    }
    release_segment_to_free(RegionKind::COLD, victim);
    return true;
}

bool FairyWrenCache::force_reclaim(RegionKind kind) {
    switch (kind) {
    case RegionKind::FWLOG:
        return reclaim_from_fwlog() || reclaim_from_hot() || reclaim_from_cold();
    case RegionKind::HOT:
        return reclaim_from_hot() || reclaim_from_cold();
    case RegionKind::COLD:
        return reclaim_from_cold();
    default:
        fw_assert(false, "unknown region kind in force_reclaim");
        return false;
    }
}

void FairyWrenCache::print_stats() {
    if (!fp_stats) {
        return;
    }
    if (static_cast<uint64_t>(write_size_to_cache) < next_stats_print_bytes_) {
        return;
    }
    const std::string& prefix = stats_prefix();
    const char* prefix_cstr = prefix.empty() ? "FAIRYWREN" : prefix.c_str();
    uint64_t total_cache_size = total_cache_block_count_ * static_cast<uint64_t>(cache_block_size_);
    auto fwlog_valid = region_valid_blocks_[region_index(RegionKind::FWLOG)];
    auto hot_valid   = region_valid_blocks_[region_index(RegionKind::HOT)];
    auto cold_valid  = region_valid_blocks_[region_index(RegionKind::COLD)];
    std::fprintf(fp_stats,
                 "%s invalidate_blocks: %llu compacted_blocks: %llu write_size_to_cache: %lld evicted_blocks: %lld write_hit_size: %lld total_cache_size: %llu fwlog_valid: %llu hot_valid: %llu cold_valid: %llu\n",
                 prefix_cstr,
                 static_cast<unsigned long long>(invalidate_blocks_),
                 static_cast<unsigned long long>(migrated_blocks_),
                 write_size_to_cache,
                 evicted_blocks,
                 write_hit_size,
                 static_cast<unsigned long long>(total_cache_size),
                 static_cast<unsigned long long>(fwlog_valid),
                 static_cast<unsigned long long>(hot_valid),
                 static_cast<unsigned long long>(cold_valid));
    std::fflush(fp_stats);
    next_stats_print_bytes_ += STATS_PRINT_INTERVAL;
}
