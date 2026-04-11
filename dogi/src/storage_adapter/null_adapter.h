#ifndef LOGSTORE_NULL_ADAPTER_H
#define LOGSTORE_NULL_ADAPTER_H

#include <sys/types.h>
#include "src/storage_adapter/storage_adapter.h"

// No-op storage adapter used when DOGI is embedded in cache_sim.
// All I/O is discarded; only DOGI's in-memory metadata matters.
class NullAdapter : public StorageAdapter {
public:
    NullAdapter() = default;
    void Write(const void *, int, off64_t) override {}
    void CreateSegment(int) override {}
    void Read(void *, int, off64_t) override {}
    void ReadWholeSegment(void *, int) override {}
    void DestroySegment(int) override {}
};

#endif // LOGSTORE_NULL_ADAPTER_H
