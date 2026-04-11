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

const uint64_t kBlockBytes    = 4096;
const uint64_t kSegmentBlocks = 98304;    // 384 MiB / 4 KiB (matches cache_sim default segment size)
const uint64_t kSegmentBytes  = kBlockBytes * kSegmentBlocks;
