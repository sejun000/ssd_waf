#ifndef GLOBALS_H
#define GLOBALS_H

#include <stdint.h>  // uint32_t, uint64_t
#include <stdio.h>   // FILE*
#include <array>

// Global configuration knobs shared across app components.
extern char *PlacementName;
extern double GpThreshold;
extern double OpRatio;
extern int LogicalSizeGb;
extern char wk_name[128];
extern uint32_t NumGroup;
extern uint32_t naive_start;
extern uint64_t BIR[10];
extern int APPLY_ML;

// Device paths/names for zoned backend.
extern const char kZnsDevicePath[];
extern const char kZbdDeviceName[];

// DogiSegment/Block size constants (configured in global.cc)
extern const uint64_t kBlockBytes;
extern const uint64_t kSegmentBlocks;         // blocks per segment
extern const uint64_t kSegmentBytes;          // bytes per segment

// Derived thresholds based on logical size.
inline uint64_t GetPassTimeBlocks() {
  return static_cast<uint64_t>(LogicalSizeGb) * 1000ull * 1000ull * 1000ull / kBlockBytes;
}
// Read optimization: global read tracker for GC relocation adjustment
#include <unordered_map>
extern bool g_dogi_read_opt;
extern bool g_dogi_read_opt_colder;
extern bool g_dogi_read_opt_hotter;
extern std::unordered_map<uint32_t, uint64_t> g_dogi_last_read_ts;

// Cross-implementation DOGI comparison telemetry (cumulative counters).
extern uint64_t g_dogi_host_hot_writes;
extern uint64_t g_dogi_host_cold_writes;
extern uint64_t g_dogi_gc_frozen_writes;
extern uint64_t g_dogi_gc_nonfrozen_writes;
extern std::array<uint64_t, 40> g_dogi_host_active_counts;
extern std::array<uint64_t, 40> g_dogi_gc_active_counts;
extern std::array<uint64_t, 40> g_dogi_gc_victim_class_counts;
extern std::array<uint64_t, 3>  g_nodap_victim_case_counts;
extern std::array<std::array<uint64_t, 40>, 3> g_nodap_victim_case_x_class_counts;
extern std::array<uint64_t, 12> g_dogi_host_age_bucket_counts;
extern std::array<uint64_t, 12> g_dogi_host_est_bucket_counts;
extern std::array<uint64_t, 12> g_dogi_victim_age_bucket_counts;
extern std::array<uint64_t, 11> g_dogi_victim_valid_ratio_bucket_counts;

#endif // GLOBALS_H
