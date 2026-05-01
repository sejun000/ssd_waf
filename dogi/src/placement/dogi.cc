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

  // NO_ML-only: read hint → age cascade(+1) 위에 한 단계 더 cold(+1) 누적.
  // 상한은 NumGroup-2 (frozen 직전). frozen filter 결정은 그대로 둠.
  if (APPLY_ML == 0 && g_dogi_read_opt_colder &&
      g_dogi_last_read_ts.count(blockAddr)) {
    int colder = next + 1;
    int cap = static_cast<int>(NumGroup) - 2;
    if (colder > cap) colder = cap;
    next = colder;
  }

  // NO_ML-only: colder의 대칭. read hint → age cascade 위에 한 단계 hotter(-1).
  // 하한은 GC 최하위 group 2 (host 영역 0,1 침범 금지).
  if (APPLY_ML == 0 && g_dogi_read_opt_hotter &&
      g_dogi_last_read_ts.count(blockAddr)) {
    int hotter = next - 1;
    if (hotter < 2) hotter = 2;
    next = hotter;
  }

  return next;
}

void DOGI::Append(uint32_t blockAddr, uint64_t timestamp) {
}

void DOGI::GcAppend(uint32_t blockAddr) {
}

void DOGI::CollectSegment(DogiSegment *segment) {
}
