#include "midas_cache.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <deque>
#include <unordered_set>
#include <algorithm>
#include <atomic>
#include <vector>
#include <unordered_map>

#include "MiDAS/ssd_config.h"
#include "MiDAS/ssdsimul.h"
#include "MiDAS/algorithm.h"
#include "MiDAS/model.h"
#include "MiDAS/hf.h"
// Avoid macro collision with OP_TYPE::WRITE/REMOVE
#undef WRITE
#undef REMOVE

namespace midas {
extern SSD_SPEC *ssd_spec;
extern SSD *ssd;
extern STATS *stats;
extern std::deque<int> fbqueue;
extern mini_model *mmodel;
extern G_INFO *ginfo;
extern MODEL_Q *model_q;
extern HotFilter *hf_gen;
extern char *workload;
extern char *vs_policy;
}

namespace {
constexpr int kDefaultTw = 4;
}

void MidasCache::RemapTable::init(std::size_t total_blocks_in) {
    total_blocks = total_blocks_in;
    mapping.clear();
    rev_mapping.clear();
    used.assign(total_blocks, 0);
    free_list.clear();
    free_list.reserve(total_blocks);
    for (std::size_t i = 0; i < total_blocks; ++i) {
        free_list.push_back(static_cast<int>(i));
    }
    ready = (total_blocks > 0);
}

int MidasCache::RemapTable::next_free() {
    while (!free_list.empty()) {
        int blk = free_list.back();
        free_list.pop_back();
        if (!used[static_cast<std::size_t>(blk)]) {
            return blk;
        }
    }
    return -1;
}

int MidasCache::RemapTable::get_or_assign(long key) {
    if (!ready || total_blocks == 0) return -1;
    auto it = mapping.find(key);
    if (it != mapping.end()) return it->second;

    std::size_t candidate = static_cast<std::size_t>(key % static_cast<long>(total_blocks));
    int mapped = -1;
    if (!used[candidate]) {
        mapped = static_cast<int>(candidate);
    } else {
        mapped = next_free();
        if (mapped == -1) {
            assert(false);
            //mapped = static_cast<int>(candidate); // fallback: reuse candidate
        }
    }
    if (mapped >= 0) {
        used[static_cast<std::size_t>(mapped)] = 1;
        mapping[key] = mapped;
        rev_mapping[mapped] = key;
    }
    return mapped;
}

int MidasCache::RemapTable::remove(long key) {
    if (!ready || total_blocks == 0) return -1;
    auto it = mapping.find(key);
    if (it == mapping.end()) {
        assert(false);
    } 
    int mapped = it->second;
    mapping.erase(it);
    rev_mapping.erase(mapped);
    if (mapped >= 0 && static_cast<std::size_t>(mapped) < used.size() && used[static_cast<std::size_t>(mapped)]) {
        used[static_cast<std::size_t>(mapped)] = 0;
        free_list.push_back(mapped);
    }
    else {
        assert(false);
    }
    return mapped;
}

int MidasCache::RemapTable::remove_by_mapped(int mapped) {
    if (!ready || total_blocks == 0) return -1;
    auto rit = rev_mapping.find(mapped);
    if (rit == rev_mapping.end()) return -1;
    long key = rit->second;
    return remove(key);
}

MidasCache::MidasCache(uint64_t              cold_capacity,
                       uint64_t              cache_block_count,
                       int                   blk_sz,
                       bool                  cache_trace,
                       const std::string&    trace_file,
                       const std::string&    cold_trace,
                       std::string&          waf_log_file,
                       const MidasConfig*    cfg,
                       IStream *input_stream_policy,
                       double input_target_valid_blk_rate,
                       std::string stat_log_file,
                       int group_count,
                       const MidasInitArgs& midas_init_args)
    : ICache(cold_capacity, waf_log_file, stat_log_file),
      cache_block_size(blk_sz),
      cfg_(cfg ? *cfg : MidasConfig{}),
      group_num(group_count),
      midas_args_(midas_init_args),
      target_valid_blk_rate(input_target_valid_blk_rate),
      valid_blk_rate_hard_limit(0.93),
      cache_trace_(cache_trace)
{
    (void)trace_file;
    (void)cold_trace;

    segment_size_blocks = cfg_.segment_bytes / blk_sz;
    total_segments = segment_size_blocks ? (cache_block_count / segment_size_blocks) : 0;
    total_cache_block_count = cache_block_count;
    total_capacity_bytes = cache_block_count * blk_sz;
    if (segment_size_blocks == 0 || total_segments == 0) {
        throw std::runtime_error("MidasCache: invalid segment sizing");
    }

    global_valid_blocks = 0;

    initialize_midas();
}

MidasCache::~MidasCache() {
    destroy_midas();
}

bool MidasCache::exists(long key) {
    (void)key;
    return false;
}

int MidasCache::get_block_size() { return cache_block_size; }

bool MidasCache::is_cache_filled() { return false; }

void MidasCache::evict_one_block() { evict_one_segment(); }

void MidasCache::evict(LogCacheSegment::Block &blk) { blk.valid = false; }

void MidasCache::print_objects(std::string /*prefix*/, uint64_t /*value*/) {}

void MidasCache::print_group_stats() {}

void MidasCache::maybe_finish_epoch() {}

void MidasCache::batch_insert(int stream_id, const std::map<long,int>& newBlocks, OP_TYPE op_type) {
    if (op_type == OP_TYPE::READ || newBlocks.empty()) return;

    for (auto [key, lba_sz] : newBlocks) {
        maybe_run_gc_policy();

        int mapped = remap_.get_or_assign(key);
        if (mapped < 0) {
            printf("failed\n");  
            continue;
        }
        write_size_to_cache += lba_sz;

        // delegate the real write to MiDAS engine (4K per call)
        midas::write(static_cast<int>(mapped), midas_ssd, midas_stats, midas_group.data(), false);
        // sync valid blocks from MiDAS atomic
        global_valid_blocks = midas::valid_pages_global.load(std::memory_order_relaxed);
    }
}

int MidasCache::invalidate(long key, int /*lba_sz*/) {
    return 0;
}

void MidasCache::evict_one_segment() {
    if (!midas_initialized || midas::ssd_spec->PPS <= 0) return;
    int victim_gid = -1;
    int victim_seg = -1;
    for (int gid = midas::ssd_spec->GROUPNUM - 1; gid >= 0; --gid) {
        if (!midas_group[gid]) continue;
        if (!midas_group[gid]->fill_queue.empty()) {
            victim_gid = gid;
            victim_seg = midas_group[gid]->fill_queue.back();
            midas_group[gid]->fill_queue.pop_back();
            break;
        }
    }
    if (victim_seg == -1) return;

    int removed = purge_segment_valids(victim_seg);
    int victim_idx = victim_seg * midas::ssd_spec->PPS;
    midas::initialize_segment(midas_ssd, victim_idx, victim_seg, victim_gid);
    midas::fbqueue.push_front(victim_seg);
    evicted_blocks += static_cast<long long>(removed);
    global_valid_blocks = midas::valid_pages_global.load(std::memory_order_relaxed);
}

void MidasCache::print_stats() {
    static uint64_t written_window_bytes = cfg_.print_stats_interval;
    static uint64_t next_written_bytes = cfg_.segment_bytes;
    if (static_cast<uint64_t>(write_size_to_cache) >= next_written_bytes) {
        compacted_blocks = midas::compacted_blocks_global.load(std::memory_order_relaxed);
    const std::string& prefix = stats_prefix();
        const char* prefix_cstr = prefix.empty() ? "LOG_CACHE" : prefix.c_str();
        fprintf (fp_stats, "%s invalidate_blocks: %lu compacted_blocks: %lu global_valid_blocks: %lu write_size_to_cache: %llu evicted_blocks: %llu write_hit_size: %llu total_cache_size: %lu reinsert_blocks: %lu read_blocks_in_partial_write %lu\n",
                 prefix_cstr,
                 static_cast<unsigned long>(invalidate_blocks),
                 static_cast<unsigned long>(compacted_blocks),
                 static_cast<unsigned long>(global_valid_blocks),
                 static_cast<unsigned long long>(write_size_to_cache),
                 static_cast<unsigned long long>(evicted_blocks),
                 static_cast<unsigned long long>(write_hit_size),
                 static_cast<unsigned long>(total_capacity_bytes),
                 static_cast<unsigned long>(reinsert_blocks),
                 static_cast<unsigned long>(read_blocks_in_partial_write));
        fflush(fp_stats);
        next_written_bytes += written_window_bytes;
    }
}

void MidasCache::initialize_midas() {
    if (midas_initialized) return;

    midas_ssd = static_cast<midas::SSD*>(malloc(sizeof(midas::SSD)));
    midas_stats = static_cast<midas::STATS*>(malloc(sizeof(midas::STATS)));

    int dev_gb = (midas_args_.dev_gb > 0)
        ? midas_args_.dev_gb
        : static_cast<int>(total_capacity_bytes / (1000 * 1000 * 1000));
    if (dev_gb <= 0) dev_gb = 1;
    int seg_mb = (midas_args_.seg_mb > 0)
        ? midas_args_.seg_mb
        : static_cast<int>(cfg_.segment_bytes / (1024 * 1024));
    if (seg_mb <= 0) seg_mb = 1;

    int gnum = (group_num > 0) ? group_num : 1;
    int naive_start = (gnum > 1) ? 1 : 0;
    int queue_gnum = naive_start + 1;
    std::vector<int> group_size(static_cast<std::size_t>(queue_gnum), 0);
    group_size[queue_gnum - 1] = -1;

    workload_str_ = midas_args_.workload.empty() ? "midas_cache_adapter" : midas_args_.workload;
    vs_policy_str_ = midas_args_.vs_policy.empty() ? "fifo" : midas_args_.vs_policy;
    midas::workload = const_cast<char*>(workload_str_.c_str());
    midas::vs_policy = const_cast<char*>(vs_policy_str_.c_str());

    int policy = midas::policy_to_int(midas::vs_policy);
    midas::ssd_init(midas_ssd, midas_group.data(), gnum, policy, naive_start, dev_gb, seg_mb);
    remap_.init(static_cast<std::size_t>(midas::ssd_spec->LBANUM));
    midas::group_init(midas_ssd, midas_group.data(), queue_gnum, 1, group_size.data());
    midas::stats_init(midas_stats);
    midas::stats = midas_stats;
    midas::ssd = midas_ssd;
    midas::hf_init(midas_ssd, &midas::hf_gen);
    if (midas::hf_gen) {
        midas::hf_gen->make_flag = 1;
        midas::hf_gen->use_flag = 1;
    }
    midas_stats->tw = dev_gb * kDefaultTw;

    midas::mmodel = static_cast<midas::mini_model*>(malloc(sizeof(midas::mini_model)));
    midas::model_create(midas::mmodel, midas_stats->tw, 0);

    midas_initialized = true;
}

void MidasCache::destroy_midas() {
    if (!midas_initialized) return;
    if (midas::mmodel) {
        midas::model_destroy(midas::mmodel);
        midas::mmodel = nullptr;
    }
    free(midas_ssd);
    free(midas_stats);
    midas_ssd = nullptr;
    midas_stats = nullptr;
    midas_initialized = false;
}

void MidasCache::maybe_run_gc_policy() {
    if (!midas_initialized) return;

    const int ILOOP = 25;
    int t = 0;
    const std::size_t low_water = std::max<std::size_t>(
        static_cast<std::size_t>(std::ceil(static_cast<double>(total_segments) * cfg_.free_ratio_low)),
        static_cast<std::size_t>(midas::ssd_spec->FREENUM));
    while (midas::fbqueue.size() < low_water) {
        double global_valid_rate = (total_cache_block_count == 0)
        ? 0.0
        : static_cast<double>(global_valid_blocks) / static_cast<double>(total_cache_block_count);
        double effective_target = (target_valid_blk_rate > 0.0)
        ? std::min(target_valid_blk_rate, valid_blk_rate_hard_limit)
        : 0.0;
        if (effective_target > 0.0 && global_valid_rate > effective_target) {
            evict_one_segment();
        }
        else {
            //printf("GC\n");
            midas::GC(midas_ssd, midas_stats, midas_group.data());
        }
        t++;
        if (t % ILOOP == 0 && t > 1) {
            break;
        }
    }
}

int MidasCache::purge_segment_valids(int victim_seg) {
    if (!midas_ssd || !midas_stats || victim_seg < 0) return 0;
    const long base = static_cast<long>(victim_seg) * midas::ssd_spec->PPS;
    int removed = 0;
    for (int i = 0; i < midas::ssd_spec->PPS; ++i) {
        long idx = base + i;
        if (idx < 0) continue;
        if (midas_ssd->itable[idx] == false && midas_ssd->oob[idx].lba != -1) { // valid page
            int lba = midas_ssd->oob[idx].lba;
            uint64_t start_index = static_cast<uint64_t>(lba) / static_cast<uint64_t>(cfg_.evicted_blk_size);
            start_index *= static_cast<uint64_t>(cfg_.evicted_blk_size);
            _evict_one_block(start_index * cache_block_size,
                             cache_block_size * static_cast<uint64_t>(cfg_.evicted_blk_size),
                             OP_TYPE::WRITE);
            remap_.remove_by_mapped(lba);
            if (lba >= 0 && midas_ssd->mtable[lba] == idx) {
                midas_ssd->mtable[lba] = -1;
            }
            removed++;
        }
    }
    if (removed > 0) {
        if (midas_stats->vp >= static_cast<unsigned long long>(removed)) {
            midas_stats->vp -= static_cast<unsigned long long>(removed);
        } else {
            midas_stats->vp = 0;
        }
        midas::valid_pages_global.store(midas_stats->vp, std::memory_order_relaxed);
    }
    return removed;
}
