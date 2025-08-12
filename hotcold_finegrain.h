#pragma once
#include <cstdint>
#include "istream.h"
#include "segment.h"
class IStream;
class HotColdFineGrain : public IStream {
public:
    HotColdFineGrain();
    int Classify(uint64_t blockAddr, bool isGcAppend, uint64_t global_timestamp, uint64_t created_timestamp) override;
    void Append(uint64_t blockAddr, uint64_t global_timestamp, void *arg) override;
    void GcAppend(uint64_t blockAddr) override;
    void CollectSegment(Segment *segment, uint64_t global_timestamp) override;
private:
};