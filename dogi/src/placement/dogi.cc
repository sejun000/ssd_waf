#include "src/placement/dogi.h"
#include "src/logstore/manager.h"
#include "app/classifier.h"
#include "app/global.h"
#include "app/group_optimizer.h"
#include <algorithm>

DOGI::DOGI() {}

int DOGI::Classify(uint32_t blockAddr, bool isGcAppend, uint64_t /*Age*/, uint32_t PrevClass, int category) {
  static uint64_t fgPrintCounter = 0;
  static uint64_t gcPrintCounter = 0;
  constexpr uint64_t kPrintEvery = 1000; // print every N classifications when APPLY_ML is on

  if (!isGcAppend) {
    int mapped = MapCategoryToGroup(category); // user-provided category
    if (mapped < 0) mapped = 0;
    if (mapped >= static_cast<int>(NumGroup)) mapped = static_cast<int>(NumGroup) - 1;
    if (APPLY_ML && (++fgPrintCounter % kPrintEvery == 0)) {
      //printf("category: %d, Group: %d\n", category, mapped){
    }
    return mapped;
  }

  uint64_t filterValue = FrozenFilterManager::Instance().Query(blockAddr);
  if (filterValue == (uint64_t)0) {
    // frozen -> send to frozen group (original behavior)
    ++g_dogi_gc_frozen_writes;
    return NumGroup - 1;
  }
  ++g_dogi_gc_nonfrozen_writes;

  // 'category' carries category on first GC; otherwise -1 for later GCs
  int next = MapCategoryFirstGc(category, static_cast<int>(PrevClass));

  // Read optimization: if block was read since last write → go hotter (-1).
  // Stay within GC groups only (group 2+), don't invade user write groups (0, 1).
  if (g_dogi_read_opt && g_dogi_last_read_ts.count(blockAddr)) {
    next = std::max(static_cast<int>(PrevClass) - 1, 2);
  }

  return next;
}

void DOGI::Append(uint32_t blockAddr, uint64_t timestamp) {
}

void DOGI::GcAppend(uint32_t blockAddr) {
}

void DOGI::CollectSegment(DogiSegment *segment) {
}
