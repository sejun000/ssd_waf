// ============================================================================
// page_mapping_ftl.cpp (implementation – corrected page‑span calculation)
// ============================================================================
#include "ftl.h"
#include <algorithm>
#include <cassert>
#include <iostream>
#include <string>

// ---------------- Block helpers ----------------
Block::Block(u64 id_) : id(id_), valid(PAGES_PER_BLOCK,false) {}

bool Block::Full() const { return writePtr >= PAGES_PER_BLOCK; }

u64 Block::NextPpn() const { return id * PAGES_PER_BLOCK + writePtr; }


// ---------------- GC: greedy -------------------
u64 GreedyGcPolicy::ChooseVictim(const std::vector<Block>& blocks,
                                 const std::vector<u64>& /*freePool*/) {
                                    
    if (minHeap.empty()) assert(false); // should not be called if no valid blocks
    auto top = minHeap.top();
    minHeap.pop();
    assert (blocks[top.id].isFree == false);
    return top.id; // return victim block ID
}

// ---------------- FTL ctor ---------------------
PageMappingFTL::PageMappingFTL(u64 totalBytes, IGcPolicy* policy, std::string wafLogFileName)
    : blocks_(), gcPolicy_(std::move(policy)) {
    u64 totalBlocks = totalBytes / NAND_BLOCK_SIZE;
    blocks_.reserve(totalBlocks);
    for (u64 i=0;i<totalBlocks;++i) blocks_.emplace_back(i);
    for (u64 i=0;i<totalBlocks;++i) freePool_.push_back(i);
    AllocateNewActiveBlock(0);
    wafLogFile = fopen(wafLogFileName.c_str(), "w");
    if (wafLogFile == NULL) {
        assert(false);
    }
    nand_write_pages = 0;
    host_write_pages = 0;
    next_write_pages = ONE_GB;
    //gc_active_block_id = NOT_ALLOCATED;
}

// ---------------- invalidate helper ------------
void PageMappingFTL::InvalidatePpn(u64 ppn) {
    u64 blkId   = ppn / PAGES_PER_BLOCK;
    u64 pageIdx = ppn % PAGES_PER_BLOCK;
    Block& blk = blocks_[blkId];
    if (blk.valid[pageIdx]) {
        blk.valid[pageIdx] = false;
        --blk.validCount;
        gcPolicy_->OnValidChange(blkId, blk.validCount);
    }
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
    PrintWafs();

    for (u64 i=0;i<numPages;++i) {
        u64 curLpn = startLpn + i;
        auto old = lpnToPpn_.find(curLpn);
        if (old != lpnToPpn_.end()) InvalidatePpn(old->second);

        u64 blkId = GetOrAllocateActiveBlock(streamId);
        if (blocks_[blkId].Full()) {
            assert(false);
        }
        Block& ab = blocks_[blkId];
        u64 ppn = ab.NextPpn();
        ab.valid[ab.writePtr] = true;
        ++ab.validCount;
        ++ab.writePtr;
        gcPolicy_->OnValidChange(ab.id, ab.validCount);
        ab.isFree = false;
        lpnToPpn_[curLpn] = ppn;

        if (freePool_.size() < GC_TRIGGER_THRESHOLD) RunGC();
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
        auto it = lpnToPpn_.find(curLpn);
        if (it==lpnToPpn_.end()) continue;
        InvalidatePpn(it->second);
        lpnToPpn_.erase(it);
    }
}

// ---------------- stats ------------------------
void PageMappingFTL::PrintStats() const {
    std::cout << "=== FTL ===\n"
              << "Mapped pages : " << lpnToPpn_.size()            << "\n"
              << "Free blocks  : " << freePool_.size()             << "\n"
              << "Used blocks  : " << TOTAL_BLOCKS - freePool_.size() << "\n";
}

void PageMappingFTL::PrintWafs() {
    if (next_write_pages < host_write_pages) {
        next_write_pages += ONE_GB;
        fprintf(wafLogFile, "hostWrite %ld nandWrite %ld\n", host_write_pages, nand_write_pages);
    }
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
    if (freePool_.empty()) RunGC();
    if (freePool_.empty()) throw std::runtime_error("Out of space even after GC");
    u64 blkId = freePool_.back();
    freePool_.pop_back();
    Block& b = blocks_[blkId];
    b.writePtr = 0;
    b.validCount = 0;
    b.isFree = false;
    gcPolicy_->OnValidChange(blkId, 0); // notify GC policy of new block
    std::fill(b.valid.begin(), b.valid.end(), false);
    activeBlk_[streamId] = blkId;
    return blkId;
}

u64 PageMappingFTL::  AllocateNewGCActiveBlock(int streamId) {
    if (freePool_.empty()) assert(false);
    if (freePool_.empty()) throw std::runtime_error("Out of space even after GC");
    u64 blkId = freePool_.back();
    freePool_.pop_back();
    Block& b = blocks_[blkId];
    b.writePtr = 0;
    b.validCount = 0;
    b.isFree = false;
    gcPolicy_->OnValidChange(blkId, 0); // notify GC policy of new block
    std::fill(b.valid.begin(), b.valid.end(), false);
    gcActiveBlk_[streamId] = blkId;
    return blkId;
}


// ---------------- Garbage Collection ----------
void PageMappingFTL::RunGC() {
    u64 victimId = gcPolicy_->ChooseVictim(blocks_, freePool_);
    if (victimId == TOTAL_BLOCKS) return;            // no victim
    if (freePool_.empty()) return;                   // no spare block

    // Allocate destination (spare) block


    // Migrate live pages
    Block& src = blocks_[victimId];
    for (u64 idx = 0; idx < PAGES_PER_BLOCK; ++idx) {
        if (!src.valid[idx]) continue;
        u64 blkId =  GetOrAllocateGCActiveBlock(0);
        if (blocks_[blkId].Full()) {
            assert(false);
        }
        Block& dest = blocks_[blkId];
        nand_write_pages++;
        u64 oldPpn = src.id * PAGES_PER_BLOCK + idx;
        u64 lpn    = FindLpnByPpn(oldPpn);
        if (lpn == static_cast<u64>(-1)) continue; // should not happen

        // copy to dest
        u64 newPpn = dest.id * PAGES_PER_BLOCK + dest.writePtr;
        dest.valid[dest.writePtr] = true;
        ++dest.validCount;
        gcPolicy_->OnValidChange(dest.id, dest.validCount);
        
        ++dest.writePtr;
        lpnToPpn_[lpn] = newPpn;
    }

    // Erase source block
    src.isFree     = true;
    src.writePtr   = 0;
    src.validCount = 0;
    gcPolicy_->OnValidChange(src.id, 0);
    std::fill(src.valid.begin(), src.valid.end(), false);
    freePool_.push_back(victimId);

    // Optionally: reclaim dest block if not fully used and almost empty etc.
}

// ---------------- Reverse lookup --------------
u64 PageMappingFTL::FindLpnByPpn(u64 ppn) const {
    for (const auto& kv : lpnToPpn_) if (kv.second == ppn) return kv.first;
    return static_cast<u64>(-1);
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
        ftl.Write(lba, PAGE_SIZE, i % 3);
        if (i % 50 == 0) ftl.Trim(lba, PAGE_SIZE);
    }
    ftl.PrintStats();
    return 0;
}
#endif