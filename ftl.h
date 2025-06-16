// ============================================================================
// page_mapping_ftl.h (header – CamelCase API, v2)
// ============================================================================
#pragma once

#include <cstdint>
#include <vector>
#include <unordered_map>
#include <memory>
//#include <queue>
#include <boost/heap/d_ary_heap.hpp>

// ---------------------------------------------------------------------------
// Tunable geometry parameters (override before including if you wish)
// ---------------------------------------------------------------------------
#ifndef PAGE_SIZE
#define PAGE_SIZE          (4 * 1024)            // bytes – 4 KiB mapping granularity
#endif
#ifndef SECTOR_SIZE
#define SECTOR_SIZE        512                   // bytes – typical logical sector (LBA)
#endif
#ifndef NAND_BLOCK_SIZE
#define NAND_BLOCK_SIZE         (32 * 1024 * 1024)    // bytes – 32 MiB physical block
#endif
#ifndef TOTAL_BLOCKS
#define TOTAL_BLOCKS       1024                  // 1024 × 32 MiB ≈ 32 GiB SSD
#endif
#define NOT_ALLOCATED 0xFFFFFFFFFFFFFFFFULL
#define ONE_GB (1 * 1024ULL * 1024ULL * 1024ULL / PAGE_SIZE)
#define GC_TRIGGER_THRESHOLD (4)
// ---------------------------------------------------------------------------
// Derived constants (do not touch)
// ---------------------------------------------------------------------------
static constexpr uint64_t SECTORS_PER_PAGE = PAGE_SIZE / SECTOR_SIZE;
static constexpr uint64_t PAGES_PER_BLOCK  = NAND_BLOCK_SIZE / PAGE_SIZE;

using u64 = uint64_t;

// ---------------------------------------------------------------------------
// Block metadata
// ---------------------------------------------------------------------------
struct Block {
    u64 id;
    std::vector<bool> valid;
    u64 writePtr    = 0;
    u64 validCount  = 0;
    bool isFree     = true;

    explicit Block(u64 id_);

    bool Full() const;
    u64  NextPpn() const;
};


struct HeapNode {
    u64 validCount;   // validCount
    u64 id;

    bool operator>(const HeapNode& o) const { return validCount > o.validCount; }
};

// ---------------------------------------------------------------------------
// GC policy interface
// ---------------------------------------------------------------------------
class IGcPolicy {
public:
    virtual ~IGcPolicy() = default;
    virtual u64 ChooseVictim(const std::vector<Block>& blocks,
                             const std::vector<u64>& freePool) = 0; // return TOTAL_BLOCKS if none
    virtual void OnValidChange(uint64_t id, uint64_t newLive) = 0; // called when valid count changes
};

class GreedyGcPolicy final : public IGcPolicy {
public:
    using Heap = boost::heap::d_ary_heap<
                    HeapNode,
                    boost::heap::arity<4>,
                    boost::heap::mutable_<true>,
                    boost::heap::compare<std::greater<>>>;
    using Handle = Heap::handle_type;
    Heap minHeap;
    std::vector<Handle> handle; 
    GreedyGcPolicy() : handle(TOTAL_BLOCKS, Heap::handle_type()) {}
    u64 ChooseVictim(const std::vector<Block>& blocks,
                     const std::vector<u64>& freePool) override;

    void OnValidChange(uint64_t id, uint64_t newLive) {
        if (handle[id] == Heap::handle_type()) {            // 아직 없음
            if (newLive > 0)
                handle[id] = minHeap.push({newLive, id});
            return;
        }
        if (newLive <= 0) {                   // ★ 즉시 삭제
            minHeap.erase(handle[id]);
            handle[id] = Heap::handle_type();
        } else {                                            // key update
            minHeap.update(handle[id], {newLive, id});
        }
        minHeap.push({newLive, id});
    }
};

// ---------------------------------------------------------------------------
// Page‑mapping FTL (log‑structured; per‑stream active block)
// ---------------------------------------------------------------------------
class PageMappingFTL {
public:
    explicit PageMappingFTL(u64 totalBytes,
                            IGcPolicy* policy, std::string wafLogFileName);

    void Write(u64 lbaOffset, u64 byteSize, int streamId);
    void Trim (u64 lbaOffset, u64 byteSize);

    void PrintStats() const;
    void PrintWafs();

private:
    // helpers
    void InvalidatePpn(u64 ppn);
    u64  AllocateNewGCActiveBlock(int streamId);
    u64  AllocateNewActiveBlock(int streamId);
    u64  GetOrAllocateGCActiveBlock(int streamId) ;
    u64  GetOrAllocateActiveBlock(int streamId);
    void RunGC();
    u64  FindLpnByPpn(u64 ppn) const;

    // state
    std::vector<Block>                       blocks_;
    std::unordered_map<u64,u64>              lpnToPpn_;
    std::vector<u64>                         freePool_;
    std::unordered_map<int,u64>              activeBlk_;
    std::unordered_map<int,u64>              gcActiveBlk_;
    std::unique_ptr<IGcPolicy>               gcPolicy_;

    u64 nand_write_pages;
    u64 host_write_pages;
    u64 next_write_pages;

    FILE *wafLogFile = NULL;
};