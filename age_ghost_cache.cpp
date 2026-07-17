#include "age_ghost_cache.h"

AgeGhostCache::AgeGhostCache(std::size_t capacity_segs)
    : capacity_segs_(capacity_segs),
      total_valid_count_(0),
      total_initial_count_(0),
      total_push_valid_count_(0),
      total_pop_valid_count_(0) {}

void AgeGhostCache::pushSegment(const std::vector<uint64_t>& blocks)
{
    if (blocks.empty()) return;
    segs_.push_back(GhostSeg{static_cast<uint64_t>(blocks.size()), blocks});
    auto new_it = std::prev(segs_.end());
    total_valid_count_ += new_it->valid_count;
    total_initial_count_ += new_it->valid_count;
    total_push_valid_count_ += new_it->valid_count;
    // No older-copy check needed: a block re-entering log cache after a flush
    // requires a host write, which already called invalidate() and removed
    // the previous lookup. So block_to_seg_ never contains b at this point.
    for (uint64_t b : new_it->blocks) {
        block_to_seg_[b] = new_it;
    }
    while (segs_.size() > capacity_segs_) {
        evict_oldest();
    }
}

bool AgeGhostCache::invalidate(uint64_t block_id)
{
    auto it = block_to_seg_.find(block_id);
    if (it == block_to_seg_.end()) return false;
    auto seg_it = it->second;
    if (seg_it->valid_count > 0) {
        seg_it->valid_count--;
        total_valid_count_--;
    }
    block_to_seg_.erase(it);
    return true;
}

void AgeGhostCache::setCapacity(std::size_t capacity_segs)
{
    capacity_segs_ = capacity_segs;
    while (segs_.size() > capacity_segs_) {
        evict_oldest();
    }
}

void AgeGhostCache::reset()
{
    segs_.clear();
    block_to_seg_.clear();
    total_valid_count_ = 0;
    total_initial_count_ = 0;
    total_push_valid_count_ = 0;
    total_pop_valid_count_ = 0;
}

void AgeGhostCache::evict_oldest()
{
    if (segs_.empty()) return;
    auto front_it = segs_.begin();
    total_pop_valid_count_ += front_it->valid_count;   // pop count = current
    total_valid_count_ -= front_it->valid_count;
    total_initial_count_ -= static_cast<uint64_t>(front_it->blocks.size());
    for (uint64_t b : front_it->blocks) {
        auto it = block_to_seg_.find(b);
        if (it != block_to_seg_.end() && it->second == front_it) {
            block_to_seg_.erase(it);
        }
    }
    segs_.pop_front();
}
