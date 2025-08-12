#include <iostream>
#include <cfloat>
#include "sepbit.h"
#include "fifo.h"
#include "metadata.h"
#include "hotcold_finegrain.h"

HotColdFineGrain::HotColdFineGrain() {

}

int HotColdFineGrain::Classify(uint64_t blockAddr, bool isGcAppend, uint64_t global_timestamp, uint64_t created_timestamp) {
  // We set global timerstamp as "time stamp diff"
  uint64_t max_lifespan = 30 * 1024ULL * 1024ULL; // 30 seconds in bytes
  if ( global_timestamp - created_timestamp > max_lifespan) {
    return IStream::MAX_STREAMS - 1; // cold stream
  }
  return (global_timestamp - created_timestamp) / (max_lifespan / IStream::MAX_STREAMS); // hot stream
}

void HotColdFineGrain::CollectSegment(Segment *segment, uint64_t global_timestamp) {
    // Do notthing
}

void HotColdFineGrain::Append(uint64_t blockAddr, uint64_t global_timestamp, void *arg) {
  // Do nothing
}

void HotColdFineGrain::GcAppend(uint64_t blockAddr) {
}
