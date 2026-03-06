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

extern uint64_t interval;

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
      cache_trace_(cache_trace),
      evicted_ages_histogram_(std::make_unique<Histogram>("midas_evicted_ages", interval/4, HISTOGRAM_BUCKETS * 2, fp_stats)),
      evicted_blocks_histogram_(std::make_unique<Histogram>("midas_evicted_blocks", 1, HISTOGRAM_BUCKETS, fp_stats)),
      evicted_ages_with_segment_histogram_(std::make_unique<Histogram>("midas_evicted_segment_age", interval/4, HISTOGRAM_BUCKETS * 2, fp_stats)),
      compacted_lifetime_histogram_(std::make_unique<Histogram>("compacted_lifetime", interval/4, HISTOGRAM_BUCKETS * 2, fp_stats))
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
    print_utilization_distribution();
    print_segment_age_scatter();
    print_inv_time_scatter();
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

        // record inv_time and compacted_lifetime for overwritten block
        if (remap_.mapping.count(key)) {
            record_inv_time(key);
            auto cit = compacted_at_.find(key);
            if (cit != compacted_at_.end()) {
                compacted_lifetime_histogram_->inc(midas_stats->cur_wp - cit->second);
                compacted_at_.erase(cit);
            }
        }

        int mapped = remap_.get_or_assign(key);
        if (mapped < 0) {
            printf("failed\n");
            continue;
        }
        write_size_to_cache += lba_sz;

        // delegate the real write to MiDAS engine (4K per call)
        midas::write(static_cast<int>(mapped), midas_ssd, midas_stats, midas_group.data(), false);
        midas::stat_update(midas_stats, 1, 0); // increment cur_wp for page_stamp tracking
        // sync valid blocks from MiDAS atomic
        global_valid_blocks = midas::valid_pages_global.load(std::memory_order_relaxed);

        if (!inv_snapshot_taken_ && write_size_to_cache >= INV_SNAPSHOT_THRESHOLD) {
            take_inv_snapshot();
        }
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
            consume_gc_compacted_lbas();
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
            // record inv_time and compacted_lifetime for GC-evicted block
            {
                auto rit = remap_.rev_mapping.find(lba);
                if (rit != remap_.rev_mapping.end()) {
                    record_inv_time(rit->second);
                    auto cit = compacted_at_.find(rit->second);
                    if (cit != compacted_at_.end()) {
                        compacted_lifetime_histogram_->inc(midas_stats->cur_wp - cit->second);
                        compacted_at_.erase(cit);
                    }
                }
            }
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
    uint64_t age = segment_age_pages(victim_seg);
    record_eviction_histograms(removed, age);
    return removed;
}

uint64_t MidasCache::segment_age_pages(int segment_idx) const {
    if (!midas_stats || !midas::ssd || !midas::ssd->seg_stamp || !midas::ssd_spec) return 0;
    if (segment_idx < 0 || segment_idx >= midas::ssd_spec->SEGNUM) return 0;
    double stamp = midas::ssd->seg_stamp[segment_idx];
    if (stamp < 0) return 0;
    double age = midas_stats->cur_wp - stamp;
    return (age > 0) ? static_cast<uint64_t>(age) : 0;
}

void MidasCache::print_utilization_distribution() {
    if (!midas_initialized || !midas_ssd || !midas::ssd_spec) return;

    static constexpr int NUM_BINS = 50;
    static constexpr double BIN_WIDTH = 2.0;
    uint64_t bins[NUM_BINS] = {};
    uint64_t total_sealed = 0;
    int PPS = midas::ssd_spec->PPS;

    for (int seg = 0; seg < midas::ssd_spec->SEGNUM; seg++) {
        if (!midas_ssd->fill_info[seg]) continue; // not sealed
        int valid = PPS - midas_ssd->irtable[seg];
        double util_pct = 100.0 * valid / PPS;
        int bin = static_cast<int>(util_pct / BIN_WIDTH);
        if (bin >= NUM_BINS) bin = NUM_BINS - 1;
        bins[bin]++;
        total_sealed++;
    }

    if (total_sealed == 0) {
        printf("[MiDAS UtilDist] no sealed segments\n");
        return;
    }

    char fname[256];
    {
        const std::string& sts = start_ts();
        const std::string& pfx = stats_prefix();
        if (sts.empty()) {
            char ts_buf[64];
            time_t now = time(nullptr); struct tm tm; localtime_r(&now, &tm);
            strftime(ts_buf, sizeof(ts_buf), "%Y%m%d_%H%M%S", &tm);
            snprintf(fname, sizeof(fname), "utilization_distribution.%s.csv", ts_buf);
        } else if (pfx.empty()) {
            snprintf(fname, sizeof(fname), "utilization_distribution.%s.csv", sts.c_str());
        } else {
            snprintf(fname, sizeof(fname), "%s.utilization_distribution.%s.csv", pfx.c_str(), sts.c_str());
        }
    }

    FILE* f = fopen(fname, "w");
    if (!f) return;
    fprintf(f, "util_low,util_high,count,fraction\n");
    for (int i = 0; i < NUM_BINS; i++) {
        double lo = i * BIN_WIDTH;
        double hi = lo + BIN_WIDTH;
        double frac = static_cast<double>(bins[i]) / total_sealed;
        fprintf(f, "%.0f,%.0f,%lu,%.6f\n", lo, hi, (unsigned long)bins[i], frac);
    }
    fclose(f);
    printf("[MiDAS UtilDist] written to %s (%lu segments)\n", fname, (unsigned long)total_sealed);
}

void MidasCache::print_segment_age_scatter() {
    if (!midas_initialized || !midas_ssd || !midas::ssd_spec || !midas_stats) return;

    const std::string& ts = start_ts();
    const std::string& prefix = stats_prefix();
    char fname[256];
    if (ts.empty()) {
        char ts_buf[64];
        time_t now = time(nullptr); struct tm tm; localtime_r(&now, &tm);
        strftime(ts_buf, sizeof(ts_buf), "%Y%m%d_%H%M%S", &tm);
        snprintf(fname, sizeof(fname), "segment_age_scatter.%s.csv", ts_buf);
    } else if (prefix.empty()) {
        snprintf(fname, sizeof(fname), "segment_age_scatter.%s.csv", ts.c_str());
    } else {
        snprintf(fname, sizeof(fname), "%s.segment_age_scatter.%s.csv", prefix.c_str(), ts.c_str());
    }

    FILE* f = fopen(fname, "w");
    if (!f) return;
    fprintf(f, "seg_age,utilization,valid_count,block_age_mean,block_age_stddev\n");

    int PPS = midas::ssd_spec->PPS;
    uint64_t cur_wp = midas_stats->cur_wp;
    uint64_t count = 0;

    for (int seg = 0; seg < midas::ssd_spec->SEGNUM; seg++) {
        if (!midas_ssd->fill_info[seg]) continue;

        int base = seg * PPS;
        int valid = 0;
        double sum = 0.0, sum_sq = 0.0;

        for (int i = 0; i < PPS; i++) {
            if (midas_ssd->itable[base + i] == false && midas_ssd->oob[base + i].lba != -1) {
                valid++;
                double age = static_cast<double>(cur_wp - midas_ssd->page_stamp[base + i]);
                sum += age;
                sum_sq += age * age;
            }
        }

        double util = static_cast<double>(valid) / PPS;
        uint64_t seg_age = (midas_ssd->seg_stamp[seg] >= 0)
            ? static_cast<uint64_t>(cur_wp - midas_ssd->seg_stamp[seg]) : 0;

        double mean = 0.0, stddev = 0.0;
        if (valid > 0) {
            mean = sum / valid;
            if (valid > 1) {
                double var = (sum_sq - sum * sum / valid) / (valid - 1);
                stddev = (var > 0.0) ? std::sqrt(var) : 0.0;
            }
        }

        fprintf(f, "%lu,%.6f,%d,%.1f,%.1f\n", (unsigned long)seg_age, util, valid, mean, stddev);
        count++;
    }

    fclose(f);
    printf("[MiDAS AgeScatter] written to %s (%lu segments)\n", fname, (unsigned long)count);
}

void MidasCache::record_eviction_histograms(int removed_pages, uint64_t segment_age_pages) {
    if (removed_pages < 0) removed_pages = 0;
    if (evicted_blocks_histogram_) {
        evicted_blocks_histogram_->inc(static_cast<uint64_t>(removed_pages));
    }
    if (evicted_ages_histogram_) {
        evicted_ages_histogram_->inc(segment_age_pages);
    }
    if (evicted_ages_with_segment_histogram_) {
        evicted_ages_with_segment_histogram_->inc(segment_age_pages);
    }
}

void MidasCache::consume_gc_compacted_lbas()
{
    if (midas::gc_compacted_lbas.empty()) return;
    uint64_t wp = midas_stats->cur_wp;
    for (int lba : midas::gc_compacted_lbas) {
        auto rit = remap_.rev_mapping.find(lba);
        if (rit != remap_.rev_mapping.end()) {
            long key = rit->second;
            // if previously compacted, record lifetime of that copy
            auto cit = compacted_at_.find(key);
            if (cit != compacted_at_.end()) {
                compacted_lifetime_histogram_->inc(wp - cit->second);
            }
            compacted_at_[key] = wp;
        }
    }
    midas::gc_compacted_lbas.clear();
}

void MidasCache::take_inv_snapshot()
{
    if (!midas_initialized || !midas_ssd || !midas::ssd_spec || !midas_stats) return;

    inv_snapshot_taken_ = true;
    inv_snapshot_wp_ = midas_stats->cur_wp;

    int PPS = midas::ssd_spec->PPS;
    uint64_t cur_wp = midas_stats->cur_wp;
    size_t seg_idx = 0;

    for (int seg = 0; seg < midas::ssd_spec->SEGNUM; seg++) {
        if (!midas_ssd->fill_info[seg]) continue;

        int base = seg * PPS;
        int valid = 0;
        double sum = 0.0, sum_sq = 0.0;

        for (int i = 0; i < PPS; i++) {
            if (midas_ssd->itable[base + i] == false && midas_ssd->oob[base + i].lba != -1) {
                valid++;
                double age = static_cast<double>(cur_wp - midas_ssd->page_stamp[base + i]);
                sum += age;
                sum_sq += age * age;
            }
        }
        if (valid == 0) continue;

        double util = static_cast<double>(valid) / PPS;
        uint64_t seg_age = (midas_ssd->seg_stamp[seg] >= 0)
            ? static_cast<uint64_t>(cur_wp - midas_ssd->seg_stamp[seg]) : 0;

        double mean = sum / valid;
        double stddev = 0.0;
        if (valid > 1) {
            double var = (sum_sq - sum * sum / valid) / (valid - 1);
            stddev = (var > 0.0) ? std::sqrt(var) : 0.0;
        }

        int gid = midas_ssd->gnum_info[seg];
        inv_snap_segs_.push_back({seg_age, util, static_cast<uint64_t>(valid), mean, stddev, gid});
        inv_snap_inv_times_.push_back({});

        // map each valid block's key -> seg_idx
        for (int i = 0; i < PPS; i++) {
            if (midas_ssd->itable[base + i] == false && midas_ssd->oob[base + i].lba != -1) {
                int lba = midas_ssd->oob[base + i].lba;
                auto rit = remap_.rev_mapping.find(lba);
                if (rit != remap_.rev_mapping.end()) {
                    inv_snap_block_seg_idx_[rit->second] = seg_idx;
                }
            }
        }
        seg_idx++;
    }

    printf("[MiDAS InvSnapshot] taken at wp=%lu, write=%.1f TB, %zu segments, %zu blocks tracked\n",
           inv_snapshot_wp_, (double)write_size_to_cache / (1024.0*1024*1024*1024),
           inv_snap_segs_.size(), inv_snap_block_seg_idx_.size());
}

void MidasCache::record_inv_time(long key)
{
    if (!inv_snapshot_taken_) return;
    auto it = inv_snap_block_seg_idx_.find(key);
    if (it != inv_snap_block_seg_idx_.end()) {
        uint64_t inv_time = midas_stats->cur_wp - inv_snapshot_wp_;
        inv_snap_inv_times_[it->second].push_back(inv_time);
        inv_snap_block_seg_idx_.erase(it);
    }
}

void MidasCache::print_inv_time_scatter()
{
    if (!inv_snapshot_taken_ || inv_snap_segs_.empty()) return;

    char fname[256];
    const std::string& ts = start_ts();
    const std::string& prefix = stats_prefix();
    if (ts.empty()) {
        char ts_buf[64];
        time_t now = time(nullptr); struct tm tm;
        localtime_r(&now, &tm);
        strftime(ts_buf, sizeof(ts_buf), "%Y%m%d_%H%M%S", &tm);
        snprintf(fname, sizeof(fname), "inv_time_scatter.%s.csv", ts_buf);
    } else if (prefix.empty()) {
        snprintf(fname, sizeof(fname), "inv_time_scatter.%s.csv", ts.c_str());
    } else {
        snprintf(fname, sizeof(fname), "%s.inv_time_scatter.%s.csv", prefix.c_str(), ts.c_str());
    }

    FILE* f = fopen(fname, "w");
    if (!f) return;

    fprintf(f, "seg_age,utilization,valid_count,age_mean,age_stddev,"
               "inv_time_mean,inv_time_stddev,inv_count,survived_count,class_num\n");

    uint64_t total_survived = 0;
    for (size_t i = 0; i < inv_snap_segs_.size(); i++) {
        auto& info = inv_snap_segs_[i];
        auto& times = inv_snap_inv_times_[i];

        uint64_t survived = info.valid_count - times.size();
        total_survived += survived;

        double inv_mean = 0.0, inv_stddev = 0.0;
        uint64_t n = times.size();
        if (n > 0) {
            double sum = 0.0, sum_sq = 0.0;
            for (auto t : times) {
                sum += (double)t;
                sum_sq += (double)t * t;
            }
            inv_mean = sum / n;
            if (n > 1) {
                double var = (sum_sq - sum * sum / n) / (n - 1);
                inv_stddev = (var > 0.0) ? std::sqrt(var) : 0.0;
            }
        }

        fprintf(f, "%lu,%.6f,%lu,%.1f,%.1f,%.1f,%.1f,%lu,%lu,%d\n",
                (unsigned long)info.seg_age, info.utilization, (unsigned long)info.valid_count,
                info.age_mean, info.age_stddev,
                inv_mean, inv_stddev, (unsigned long)n, (unsigned long)survived, info.group_id);
    }

    fclose(f);
    printf("[MiDAS InvTimeScatter] written to %s (%zu segments, %lu blocks survived)\n",
           fname, inv_snap_segs_.size(), (unsigned long)total_survived);
}
