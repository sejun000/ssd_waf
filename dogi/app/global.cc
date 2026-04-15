#include "app/global.h"

char *PlacementName = nullptr;
double GpThreshold = 0.12;                // 1 - 0.88 (cache_sim _88 target valid ratio)
double OpRatio     = 0.13636;             // 1/0.88 - 1
int LogicalSizeGb  = 128;                 // overwritten at runtime by dogi_cache init
char wk_name[128]  = "";                  // unused (cache_sim trace_parser feeds writes)
uint32_t NumGroup  = 6;
uint64_t BIR[10]   = {0,};
uint32_t naive_start = 1;
int APPLY_ML = 0;

const char kZnsDevicePath[] = "/dev/null";
const char kZbdDeviceName[] = "null";

bool g_dogi_read_opt = false;
std::unordered_map<uint32_t, uint64_t> g_dogi_last_read_ts;
uint64_t g_dogi_host_hot_writes = 0;
uint64_t g_dogi_host_cold_writes = 0;
uint64_t g_dogi_gc_frozen_writes = 0;
uint64_t g_dogi_gc_nonfrozen_writes = 0;
std::array<uint64_t, 40> g_dogi_host_active_counts = {0,};
std::array<uint64_t, 40> g_dogi_gc_active_counts = {0,};
std::array<uint64_t, 40> g_dogi_gc_victim_class_counts = {0,};

const uint64_t kBlockBytes    = 4096;
const uint64_t kSegmentBlocks = 98304;    // 384 MiB / 4 KiB (matches cache_sim default segment size)
const uint64_t kSegmentBytes  = kBlockBytes * kSegmentBlocks;
