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
bool g_dogi_read_opt_colder = false;
bool g_dogi_read_opt_hotter = false;
std::unordered_map<uint32_t, uint64_t> g_dogi_last_read_ts;
uint64_t g_dogi_host_hot_writes = 0;
uint64_t g_dogi_host_cold_writes = 0;
uint64_t g_dogi_gc_frozen_writes = 0;
uint64_t g_dogi_gc_nonfrozen_writes = 0;
std::array<uint64_t, 40> g_dogi_host_active_counts = {0,};
std::array<uint64_t, 40> g_dogi_gc_active_counts = {0,};
std::array<uint64_t, 40> g_dogi_gc_victim_class_counts = {0,};
// NoDaP score-formula case counts: case 0 = fully-valid skip (-1e18,
// shouldn't happen), case 1 = expired G_1..G_{N-1} (1e18+age),
// case 2 = greedy fallback (idx-g_n_idx)*seg_blocks - valid_cnt.
std::array<uint64_t, 3> g_nodap_victim_case_counts = {0,};
// Per-class breakdown: counts[case][class_num]. 3 cases × 40 max classes.
std::array<std::array<uint64_t, 40>, 3> g_nodap_victim_case_x_class_counts = {};
// Sum of valid_ratio per case for averaging.
std::array<double, 3> g_nodap_victim_case_valid_ratio_sum = {0.0, 0.0, 0.0};
// Currently-sealed segment count per class (gauge: ++ on seal, -- on victim pick).
std::array<int64_t, 40> g_nodap_sealed_per_class = {0,};
std::array<uint64_t, 12> g_dogi_host_age_bucket_counts = {0,};
std::array<uint64_t, 12> g_dogi_host_est_bucket_counts = {0,};
std::array<uint64_t, 12> g_dogi_victim_age_bucket_counts = {0,};
std::array<uint64_t, 11> g_dogi_victim_valid_ratio_bucket_counts = {0,};

const uint64_t kBlockBytes    = 4096;
const uint64_t kSegmentBlocks = 98304;    // 384 MiB / 4 KiB (matches cache_sim default segment size)
const uint64_t kSegmentBytes  = kBlockBytes * kSegmentBlocks;
