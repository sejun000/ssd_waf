// ============================================================================
// page_mapping_ftl.cpp (implementation – corrected page‑span calculation)
// ============================================================================
#include "ftl.h"
#include <algorithm>
#include <cassert>
#include <iostream>
#include <string>
#include <cstring> 

// ---------------- Block helpers ----------------
Block::Block(u64 id_) : id(id_), valid(PAGES_PER_BLOCK,false) {}

bool Block::Full() const { return writePtr >= PAGES_PER_BLOCK; }

u64 Block::NextPpn() const { return id * PAGES_PER_BLOCK + writePtr; }


void GreedyGcPolicy::OnValidChange(uint64_t id, uint64_t newLive) {
    assert (id < total_blocks);
    if (handle[id] == Heap::handle_type()) {            // 아직 없음
        if (newLive > 0)
            handle[id] = minHeap.push({newLive, id});
        return;
    }
    if (newLive > 0) {                                             // key update
        minHeap.update(handle[id], {newLive, id});
    }
}

// ---------------- GC: greedy -------------------
u64 GreedyGcPolicy::ChooseVictim(const std::vector<Block>& blocks,
                                 const std::vector<u64>& /*freePool*/) {
                                    
    if (minHeap.empty()) assert(false); // should not be called if no valid blocks
    auto top = minHeap.top();
    minHeap.pop();
    assert (blocks[top.id].isFree == false);
    handle[top.id] = Heap::handle_type();
    return top.id; // return victim block ID
}

// ---------------- FTL ctor ---------------------
PageMappingFTL::PageMappingFTL(u64 totalBytes, IGcPolicy* policy)
    : blocks_(), gcPolicy_(std::move(policy)) {
    u64 totalBlocks = totalBytes / NAND_BLOCK_SIZE;
    blocks_.reserve(totalBlocks);
    for (u64 i=0;i<totalBlocks;++i) blocks_.emplace_back(i);
    for (u64 i=0;i<totalBlocks;++i) freePool_.push_back(i);
    AllocateNewActiveBlock(0);
    nand_write_pages = 0;
    host_write_pages = 0;
    total_nand_pages = totalBytes / NAND_PAGE_SIZE;
    printf("total_nand_pages : %ld \n", total_nand_pages);
    lpnToPpn_ = (u64 *)calloc(total_nand_pages * 2, sizeof(u64));
    ppnToLpn_ = (u64 *)calloc(total_nand_pages, sizeof(u64));
    memset(lpnToPpn_, 0xFF, total_nand_pages * 2 * sizeof(u64));
    memset(ppnToLpn_, 0xFF, total_nand_pages * sizeof(u64));

    //gc_active_block_id = NOT_ALLOCATED;
}

// ---------------- invalidate helper ------------
void PageMappingFTL::InvalidatePpn(u64 ppn) {
    if (ppn == NOT_ALLOCATED) {
        return;
    }
    u64 blkId   = ppn / PAGES_PER_BLOCK;
    u64 pageIdx = ppn % PAGES_PER_BLOCK;
    Block& blk = blocks_[blkId];
    if (blk.valid[pageIdx]) {
        blk.valid[pageIdx] = false;
        --blk.validCount;
        gcPolicy_->OnValidChange(blkId, blk.validCount);
    }
}

u64 PageMappingFTL::GetPpn(u64 lpn) {
    assert (lpn < total_nand_pages * 2);
    return lpnToPpn_[lpn];
}

void PageMappingFTL::Unmap(u64 lpn) {
    lpnToPpn_[lpn] = NOT_ALLOCATED;
}

u64 PageMappingFTL::GetLpn(u64 ppn) {
    assert (ppn < total_nand_pages);
    return ppnToLpn_[ppn];
}

void PageMappingFTL::SetPpn(u64 lpn, u64 ppn) {
    assert (lpn < total_nand_pages * 2);
    assert (ppn < total_nand_pages);
    lpnToPpn_[lpn] = ppn;
    ppnToLpn_[ppn] = lpn;
}

u64 PageMappingFTL::GetHostWritePages() {
    return host_write_pages;
}

u64 PageMappingFTL::GetNandWritePages() {
    return nand_write_pages;
}
// ---------------- Write (fixed span calc) ------
void PageMappingFTL::Write(u64 lbaOffset, u64 byteSize, int streamId) {
    assert(lbaOffset % SECTOR_SIZE == 0 && byteSize % SECTOR_SIZE == 0);
    assert(byteSize > 0);

    const u64 firstSector = lbaOffset / SECTOR_SIZE;
    const u64 lastSector  = (lbaOffset + byteSize - 1) / SECTOR_SIZE;

    const u64 startLpn = firstSector / SECTORS_PER_PAGE;
    const u64 endLpn   = lastSector  / SECTORS_PER_PAGE;
    const u64 numPages = endLpn - startLpn + 1;          // inclusive span

    host_write_pages += numPages;
    nand_write_pages += numPages;

    for (u64 i=0;i<numPages;++i) {
        u64 curLpn = startLpn + i;
        auto old = GetPpn(curLpn);
        InvalidatePpn(old);

        u64 blkId = GetOrAllocateActiveBlock(streamId);
        if (blocks_[blkId].Full()) {
            assert(false);
        }
        Block& ab = blocks_[blkId];
        u64 ppn = ab.NextPpn();
        ab.valid[ab.writePtr] = true;
        ++ab.validCount;
        ++ab.writePtr;
        if (ab.Full()) {
            gcPolicy_->OnValidChange(ab.id, ab.validCount);
        }
        ab.isFree = false;
        SetPpn(curLpn, ppn);

        //if (freePool_.size() < GC_TRIGGER_THRESHOLD) RunGC();
    }
}

// ---------------- Trim (fixed span calc) -------
void PageMappingFTL::Trim(u64 lbaOffset, u64 byteSize) {
    assert(lbaOffset % SECTOR_SIZE == 0 && byteSize % SECTOR_SIZE == 0);
    if (byteSize == 0) return;

    const u64 firstSector = lbaOffset / SECTOR_SIZE;
    const u64 lastSector  = (lbaOffset + byteSize - 1) / SECTOR_SIZE;
    const u64 startLpn = firstSector / SECTORS_PER_PAGE;
    const u64 endLpn   = lastSector  / SECTORS_PER_PAGE;

    for (u64 curLpn = startLpn; curLpn <= endLpn; ++curLpn) {
        u64 old = GetPpn(curLpn);
        if (old == NOT_ALLOCATED) continue;
        InvalidatePpn(old);
        Unmap(curLpn);
    }
}

// ---------------- stats ------------------------
void PageMappingFTL::PrintStats() const {
    /*std::cout << "=== FTL ===\n"
              << "Mapped pages : " << lpnToPpn_.size()            << "\n"
              << "Free blocks  : " << freePool_.size()             << "\n"
              << "Used blocks  : " << TOTAL_BLOCKS - freePool_.size() << "\n";*/
}

// ---------------- helper paths -----------------
u64 PageMappingFTL::GetOrAllocateActiveBlock(int streamId) {
    auto it = activeBlk_.find(streamId);
    if (it!=activeBlk_.end() && !blocks_[it->second].Full()) return it->second;
    return AllocateNewActiveBlock(streamId);
}

// ---------------- helper paths -----------------
u64 PageMappingFTL::GetOrAllocateGCActiveBlock(int streamId) {
    auto it = gcActiveBlk_.find(streamId);
    if (it!=gcActiveBlk_.end() && !blocks_[it->second].Full()) return it->second;
    return AllocateNewGCActiveBlock(streamId);
}


u64 PageMappingFTL::AllocateNewActiveBlock(int streamId) {
    while (freePool_.size() < GC_TRIGGER_THRESHOLD) RunGC();
    if (freePool_.empty()) throw std::runtime_error("Out of space even after GC");
    u64 blkId = freePool_.back();
    freePool_.pop_back();
    Block& b = blocks_[blkId];
    b.writePtr = 0;
    b.validCount = 0;
    b.isFree = false;
    std::fill(b.valid.begin(), b.valid.end(), false);
    activeBlk_[streamId] = blkId;
    return blkId;
}

u64 PageMappingFTL::AllocateNewGCActiveBlock(int streamId) {
    if (freePool_.empty()) assert(false);
    if (freePool_.empty()) throw std::runtime_error("Out of space even after GC");
    u64 blkId = freePool_.back();
    freePool_.pop_back();
    Block& b = blocks_[blkId];
    b.writePtr = 0;
    b.validCount = 0;
    b.isFree = false;
    std::fill(b.valid.begin(), b.valid.end(), false);
    gcActiveBlk_[streamId] = blkId;
    return blkId;
}


// ---------------- Garbage Collection ----------
void PageMappingFTL::RunGC() {
    u64 victimId = gcPolicy_->ChooseVictim(blocks_, freePool_);

    // Allocate destination (spare) block

    // Migrate live pages
    Block& src = blocks_[victimId];
    for (u64 idx = 0; idx < PAGES_PER_BLOCK; ++idx) {
        if (!src.valid[idx]) continue;
        u64 blkId = GetOrAllocateGCActiveBlock(0);
        if (blocks_[blkId].Full()) {
            assert(false);
        }
        Block& dest = blocks_[blkId];
        nand_write_pages++;
        u64 oldPpn = src.id * PAGES_PER_BLOCK + idx;
        u64 lpn    = GetLpn(oldPpn);
        assert (lpn != NOT_ALLOCATED);

        // copy to dest
        u64 newPpn = dest.id * PAGES_PER_BLOCK + dest.writePtr;
        dest.valid[dest.writePtr] = true;
        ++dest.validCount;
        ++dest.writePtr;
        SetPpn(lpn, newPpn);
    }

    // Erase source block
    src.isFree     = true;
    src.writePtr   = 0;
    src.validCount = 0;
    assert (src.id == victimId);
    std::fill(src.valid.begin(), src.valid.end(), false);
    freePool_.push_back(victimId);

    // Optionally: reclaim dest block if not fully used and almost empty etc.
}

// ---------------- Demo main (compile with -DFTL_DEMO) -----
#ifdef FTL_DEMO
#include <random>
int main() {
    PageMappingFTL ftl;
    // write 2 KiB offset, 4 KiB size → should span 2 pages
    ftl.Write(4 * 512, 4096, 0);  // LBA 4 sectors = 2 KiB offset

    // random workload
    std::mt19937_64 rng(1);
    for (int i = 0; i < 10000; ++i) {
        u64 lba = (rng() % (16 * 1024)) * SECTOR_SIZE;   // up to 8 MiB range
        ftl.Write(lba, NAND_PAGE_SIZE, i % 3);
        if (i % 50 == 0) ftl.Trim(lba, NAND_PAGE_SIZE);
    }
    ftl.PrintStats();
    return 0;
}
#endif