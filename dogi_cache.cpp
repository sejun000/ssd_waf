#include "dogi_cache.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <sys/types.h>

// DOGI headers
#include "dogi/app/global.h"
#include "dogi/app/classifier.h"
#include "dogi/app/freq_features.h"
#include "dogi/app/group_config.h"
#include "dogi/app/group_optimizer.h"
#include "dogi/app/mlp_inference.h"
#include "dogi/app/model_train.h"
#include "dogi/src/logstore/manager.h"
#include "dogi/src/logstore/segment.h"
#include "dogi/src/logstore/config.h"
#include "dogi/src/selection/selection.h"
#include "dogi/src/selection/factory.h"

namespace {
// Resolve trained model dir/name from env or DOGI-Train/TrainedModel/latest pointer.
std::pair<std::string, std::string> ResolveModelDirAndName() {
    const char *envDir = std::getenv("MLP_MODEL_DIR");
    auto basename = [](const std::string &path) {
        auto pos = path.find_last_of("/\\");
        return (pos == std::string::npos) ? path : path.substr(pos + 1);
    };
    std::string modelDir;
    if (envDir) {
        modelDir = envDir;
    } else {
        const std::string latestPath = "dogi/DOGI-Train/TrainedModel/latest";
        std::ifstream latest(latestPath);
        if (latest.good()) {
            std::string name;
            std::getline(latest, name);
            name.erase(name.find_last_not_of(" \t\r\n") + 1);
            if (!name.empty()) {
                modelDir = "dogi/DOGI-Train/TrainedModel/" + name;
            }
        }
    }
    return {modelDir, basename(modelDir)};
}
} // namespace

DogiCache::DogiCache(uint64_t cold_capacity,
                     uint64_t cache_block_count,
                     int cache_block_size,
                     bool /*cache_trace*/,
                     const std::string& /*trace_file*/,
                     const std::string& /*cold_trace_file*/,
                     std::string& waf_log_file,
                     const std::string& stat_log_file)
    : ICache(cold_capacity, waf_log_file, stat_log_file),
      cache_block_size_(cache_block_size),
      cache_block_count_(cache_block_count),
      cold_capacity_(cold_capacity)
{
    // Configure DOGI globals from cache_sim parameters.
    const uint64_t cache_bytes = cache_block_count * static_cast<uint64_t>(cache_block_size);
    // LogicalSizeGb is the user-visible (logical) size in GB; cache_bytes already accounts for OP
    // because cache_sim's `capacity` is the physical cache_block_count. Convert: logical = physical / (1+OP).
    LogicalSizeGb = static_cast<int>(static_cast<double>(cache_bytes) / (1.0 + OpRatio) / 1.0e9);
    if (LogicalSizeGb <= 0) LogicalSizeGb = 1;

    // Wire DOGI Config singleton to use the in-memory adapter and DOGI placement/selection.
    Config::GetInstance().placement = "DOGI";
    Config::GetInstance().selection = "DogiSelect";
    Config::GetInstance().indexMap = "Array";
    Config::GetInstance().storageAdapter = "Null";
    Config::GetInstance().maxNumOpenSegments = static_cast<int>(NumGroup);

    PlacementName = const_cast<char*>("DOGI");

    // BIR[0] is the upper bound of Hot Filter (in blocks); paper init = 2 * kSegmentBlocks.
    BIR[0] = 2ull * kSegmentBlocks;

    printf("[DOGI] init: LogicalSizeGb=%d OpRatio=%.4f GpThreshold=%.4f NumGroup=%u kSegmentBlocks=%lu\n",
           LogicalSizeGb, OpRatio, GpThreshold, NumGroup, (unsigned long)kSegmentBlocks);

    // Construct Manager directly (no LogStore wrapper -> no buse dependency).
    manager_ = std::make_unique<Manager>(static_cast<int>(NumGroup));

    // Selection: dispatched via factory using Config; mirrors Scheduler::Scheduler() body.
    selection_.reset(SelectionFactory::GetInstance(Config::GetInstance().selection));

    // Size DOGI's per-block metadata (bitmap, freq tracker) to cover the full
    // upstream LBA range (cold_capacity), not just the cache device. Trace LBAs
    // can address the entire backing store; using cache_bytes would cause
    // out-of-bounds bitmap accesses. Memory cost: ~320 MB for 1 TB cold tier.
    const uint64_t deviceBytes = cold_capacity;
    freq_tracker_  = std::make_shared<FreqFeatures>(deviceBytes);
    model_trainer_ = std::make_unique<ModelTrainer>(deviceBytes);
    mlp_inference_ = std::make_unique<MlpInference>();

    // Hot threshold (in HotIntervalTracker units) -- paper default 2, same as main.cc.
    hot_classifier_ = std::make_unique<DogiHotClassifier>(/*hotThreshold=*/2);

    pass_time_blocks_ = GetPassTimeBlocks();

    // Try to load any pre-existing trained model (matches main.cc startup).
    {
        auto pair = ResolveModelDirAndName();
        if (!pair.first.empty()) {
            std::string err;
            if (mlp_inference_->LoadFromDir(pair.first, &err)) {
                latest_model_name_ = pair.second;
                latest_model_dir_  = pair.first;
                printf("[DOGI/MLP] Loaded model from %s\n", pair.first.c_str());
            } else {
                printf("[DOGI/MLP] Failed to load %s: %s\n", pair.first.c_str(), err.c_str());
            }
        } else {
            printf("[DOGI/MLP] No existing model — random weights until training completes\n");
        }
    }

    // Override with GCONF_BASE_DIR if user supplied; otherwise mirror main.cc default.
    const char *gconf_env = std::getenv("GCONF_BASE_DIR");
    gconf_base_ = gconf_env ? gconf_env : "../DOGI-Gconf";
}

DogiCache::~DogiCache() = default;

bool DogiCache::exists(long /*key*/) {
    // cache_sim only uses exists() for hit-ratio reporting on read paths.
    // DOGI is write-focused; reads from cache are out of scope here.
    return false;
}

std::size_t DogiCache::size() {
    return manager_ ? static_cast<std::size_t>(manager_->GetnValidBlocks()) : 0;
}

bool DogiCache::is_cache_filled() {
    if (!manager_) return false;
    // Approximate "cache filled" by checking valid block count vs configured capacity.
    return manager_->GetnValidBlocks() >= cache_block_count_ * 9 / 10;
}

void DogiCache::batch_insert(int /*stream_id*/,
                             const std::map<long, int>& newBlocks,
                             OP_TYPE op_type)
{
    if (newBlocks.empty()) return;
    if (op_type == OP_TYPE::READ) {
        // DOGI prototype's main.cc only handles writes (op==1). Skip reads.
        return;
    }

    for (auto [key, lba_sz] : newBlocks) {
        const uint32_t lba = static_cast<uint32_t>(key);
        const uint64_t segmentAge = manager_->GetSegmentAge(lba);

        ++logical_time_;
        if (logical_time_ > pass_time_blocks_) {
            FrozenFilterManager::Instance().Update(lba);
        }
        auto hotResult = hot_classifier_->Classify(lba, segmentAge);
        const bool isHot = (logical_time_ > pass_time_blocks_) ? hotResult.isHot : false;
        const uint64_t interval = static_cast<uint64_t>(hotResult.interval);

        buffer_.push_back({lba, interval, isHot});
        if (!isHot) ++non_hot_count_;
        buffer_bytes_ += static_cast<std::size_t>(lba_sz);
        write_size_to_cache += lba_sz;

        if (non_hot_count_ >= kNonHotThreshold || buffer_bytes_ >= kBufferBytesLimit) {
            flush_buffer();
        }
    }

    try_run_config_step();
}

void DogiCache::flush_buffer() {
    if (buffer_.empty()) {
        non_hot_count_ = 0;
        buffer_bytes_ = 0;
        return;
    }

    static constexpr std::size_t kBatch = MlpInference::kBatch;
    std::vector<std::array<uint64_t, MlpInference::kInputDim>> mlBatch;
    mlBatch.reserve(kBatch);

    const std::size_t cappedNonHot =
        std::min(non_hot_count_, static_cast<std::size_t>(MlpInference::kBatch));

    // Build feature vectors (mirror main.cc worker thread).
    for (auto &w : buffer_) {
        const uint8_t bits     = freq_tracker_->GetFreqBits(w.lba);
        const uint8_t bitCount = freq_tracker_->GetFreqBitCount(w.lba);
        const uint64_t segFreq = freq_tracker_->GetChunkFrequency(w.lba);
        std::array<uint64_t, MlpInference::kInputDim> feat = {
            static_cast<uint64_t>(w.lba),
            static_cast<uint64_t>(prev_lba_),
            static_cast<uint64_t>(w.interval),
            static_cast<uint64_t>(bits),
            static_cast<uint64_t>(bitCount),
            static_cast<uint64_t>(segFreq)
        };
        prev_lba_ = w.lba;
        ++feature_time_;

        const uint64_t deviceBytes =
            static_cast<uint64_t>(LogicalSizeGb) * 1000ull * 1000ull * 1000ull;
        if (feature_time_ > (deviceBytes * 2ull / 4096ull)) {
            model_trainer_->SaveTrainingData(&feat, feature_time_, w.isHot);
        }
        if (!w.isHot) {
            mlBatch.push_back(feat);
        }
    }

    static thread_local std::array<int, MlpInference::kBatch> mlPred{};
    bool havePredictions = false;
    if (!mlBatch.empty() && mlp_inference_->IsModelLoaded()) {
        if (mlBatch.size() > MlpInference::kBatch) {
            mlBatch.resize(MlpInference::kBatch);
        }
        while (mlBatch.size() < MlpInference::kBatch) {
            mlBatch.push_back(mlBatch.back());
        }
        mlp_inference_->RunBatch(mlBatch, mlPred);
        havePredictions = true;
    }

    std::size_t predIdx = 0;
    for (auto &w : buffer_) {
        int category = w.isHot ? 0 : 1;
        if (!w.isHot && havePredictions && predIdx < cappedNonHot) {
            category = mlPred[predIdx] + 1;
            ++predIdx;
        }
        // Manager::Append expects byte-addressed logical address; null adapter ignores buf.
        manager_->Append(/*buf=*/nullptr,
                         /*addr=*/static_cast<off64_t>(w.lba) * 4096,
                         /*group=*/category);
        freq_tracker_->OnBlockWrite(w.lba);
    }

    buffer_.clear();
    non_hot_count_ = 0;
    buffer_bytes_ = 0;

    // Run inline GC if invalid block fraction crossed the threshold during this batch.
    run_inline_gc();
}

void DogiCache::run_inline_gc() {
    while (manager_->GetGp() >= GpThreshold) {
        // Mirror Scheduler::scheduling() inner loop body.
        std::vector<DogiSegment> segments;
        manager_->GetSegments(segments);
        if (segments.empty()) break;
        auto res = selection_->Select(segments);
        if (res.empty()) break;
        const int sid = res[0].second;

        DogiSegment seg = manager_->ReadSegment(sid);
        manager_->CollectSegment(seg.GetSegmentId());
        uint64_t nRewrite = 0;
        for (uint64_t i = 0; i < kSegmentBlocks; ++i) {
            off64_t blockAddr = seg.GetBlockAddr(i);
            if (blockAddr == static_cast<off64_t>(UINT32_MAX)) continue;
            off64_t oldPhy = seg.GetPhyAddr(i);
            char* data = seg.GetBlockData(i);
            if (!manager_->GcAppend(data, static_cast<uint32_t>(blockAddr), oldPhy)) {
                ++nRewrite;
            }
        }
        manager_->RemoveSegment(seg.GetSegmentId(),
                                seg.GetTotalInvalidBlocks() + nRewrite);
    }
}

void DogiCache::try_run_config_step() {
    // Mirror configWorker thread: once enough samples are collected, train ML +
    // reload + run group optimizer. Run only once per training cycle.
    if (config_applied_) return;
    if (!model_trainer_->IsCollectingDone()) return;

    printf("[DOGI] training trigger fired (logical_time=%lu)\n", logical_time_);
    model_trainer_->MakingMLModel();

    auto pair = ResolveModelDirAndName();
    if (!pair.first.empty()) {
        std::string err;
        if (mlp_inference_->LoadFromDir(pair.first, &err)) {
            latest_model_name_ = pair.second.empty() ? latest_model_name_ : pair.second;
            latest_model_dir_  = pair.first;
            printf("[DOGI/MLP] Reloaded model from %s\n", pair.first.c_str());
        } else {
            printf("[DOGI/MLP] Reload failed for %s: %s\n", pair.first.c_str(), err.c_str());
        }
    }

    const double utilization = 1.0 - GpThreshold;
    bool ok = OptimizeAndApplyGroupConfig(latest_model_name_, utilization, gconf_base_);
    if (ok) {
        APPLY_ML = 1;
    }
    config_applied_ = true;
    printf("[DOGI/GCONF] group optimizer %s (APPLY_ML=%d)\n", ok ? "success" : "failed", APPLY_ML);
}

void DogiCache::print_stats() {
    // No-op: cache_sim calls print_stats() per trace line. DOGI's manager.cc
    // already self-throttles PrintRealStats() every 20 GiB inside Manager::Append.
}
