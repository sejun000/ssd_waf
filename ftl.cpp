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
Block::Block(u64 id_) : Segment(0), id(id_), valid(PAGES_PER_BLOCK,false) {}

void Block::reset() {  
    isFree = true;      
    write_ptr = 0;
    valid_cnt = 0;
}
bool Block::full() { 
    return write_ptr >= PAGES_PER_BLOCK; 
}

u64 Block::NextPpn() const { return id * PAGES_PER_BLOCK + write_ptr; }

// ---------------- FTL ctor ---------------------
PageMappingFTL::PageMappingFTL(u64 totalBytes, EvictPolicy* policy)
    : blocks_(), gcPolicy_(std::move(policy)) {
    u64 totalBlocks = totalBytes / NAND_BLOCK_SIZE;
    blocks_.reserve(totalBlocks);
    for (u64 i=0;i<totalBlocks;++i) blocks_.emplace_back(i);
    for (u64 i=0;i<totalBlocks;++i) freePool_.push_back(i);
    AllocateNewActiveBlock(0);
    nand_write_pages = 0;
    host_write_pages = 0;
    total_nand_pages = totalBytes / NAND_PAGE_SIZE;
    printf("total_nand_pages : %llu \n", (unsigned long long)total_nand_pages);

    // ▶▶ 희소 매핑: 필요한 만큼만 저장
    lpnToPpn_.reserve(total_nand_pages); // 동시에 유효한 LPN 수는 PPN 수를 넘지 않음
    ppnToLpn_.reserve(total_nand_pages);
}

// ---------------- invalidate helper ------------
void PageMappingFTL::InvalidatePpn(u64 ppn) {
    if (ppn == NOT_ALLOCATED) return;

    u64 blkId   = ppn / PAGES_PER_BLOCK;
    u64 pageIdx = ppn % PAGES_PER_BLOCK;
    Block& blk = blocks_[blkId];

    if (blk.valid[pageIdx]) {
        blk.valid[pageIdx] = false;
        --blk.valid_cnt;
        if (blk.full()) {
            gcPolicy_->update(&blk);
        }
    }
    // ▶▶ 역매핑 정리 (유효하지 않은 PPN은 더 이상 LPN을 가리키지 않음)
    ppnToLpn_.erase(ppn);
}

u64 PageMappingFTL::GetPpn(u64 lpn) {
    //assert(lpn < total_nand_pages * 2);
    auto it = lpnToPpn_.find(lpn);
    return (it == lpnToPpn_.end()) ? NOT_ALLOCATED : it->second;
}

void PageMappingFTL::Unmap(u64 lpn) {
    // LPN → PPN 제거 (Trim 용도)
    lpnToPpn_.erase(lpn);
}

u64 PageMappingFTL::GetLpn(u64 ppn) {
    assert(ppn < total_nand_pages);
    auto it = ppnToLpn_.find(ppn);
    return (it == ppnToLpn_.end()) ? NOT_ALLOCATED : it->second;
}

void PageMappingFTL::SetPpn(u64 lpn, u64 ppn) {
//    assert(lpn < total_nand_pages * 2);
    assert(ppn < total_nand_pages);
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
        if (blkId == NOT_ALLOCATED) return;
        if (blocks_[blkId].full()) {
            assert(false);
        }
        Block& ab = blocks_[blkId];
        u64 ppn = ab.NextPpn();
        ab.valid[ab.write_ptr] = true;
        ++ab.valid_cnt;
        ++ab.write_ptr;
        if (ab.full()) {
            gcPolicy_->add(&ab);
        }
        ab.isFree = false;
        SetPpn(curLpn, ppn);

        //if (freePool_.size() < GC_TRIGGER_THRESHOLD) RunGC();
    }
}

// ---------------- Trim (fixed span calc) -------
void PageMappingFTL::Trim(u64 lbaOffset, u64 byteSize) {
    if (!(lbaOffset % SECTOR_SIZE == 0 && byteSize % SECTOR_SIZE == 0)){
        printf("%lu %lu\n", lbaOffset, byteSize);
        assert(lbaOffset % SECTOR_SIZE == 0 && byteSize % SECTOR_SIZE == 0);
    }
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
    if (it!=activeBlk_.end() && !blocks_[it->second].full()) return it->second;
    return AllocateNewActiveBlock(streamId);
}

// ---------------- helper paths -----------------
u64 PageMappingFTL::GetOrAllocateGCActiveBlock(int streamId) {
    auto it = gcActiveBlk_.find(streamId);
    if (it!=gcActiveBlk_.end() && !blocks_[it->second].full()) return it->second;
    return AllocateNewGCActiveBlock(streamId);
}


u64 PageMappingFTL::AllocateNewActiveBlock(int streamId) {
    while (freePool_.size() < GC_TRIGGER_THRESHOLD) {
        std::size_t before = freePool_.size();
        RunGC();
    }
    if (freePool_.empty()) {
        printf("Out of space even after GC (active)\n");
        return NOT_ALLOCATED;
    }
    u64 blkId = freePool_.back();
    freePool_.pop_back();
    Block& b = blocks_[blkId];
    b.write_ptr = 0;
    b.valid_cnt = 0;
    b.isFree = false;
    std::fill(b.valid.begin(), b.valid.end(), false);
    activeBlk_[streamId] = blkId;
    return blkId;
}

u64 PageMappingFTL::AllocateNewGCActiveBlock(int streamId) {
    if (freePool_.empty()) assert(false);
    if (freePool_.empty()) {
        printf("Out of space even after GC (gc-active)\n");
        return NOT_ALLOCATED;
    }
    u64 blkId = freePool_.back();
    freePool_.pop_back();
    Block& b = blocks_[blkId];
    b.write_ptr = 0;
    b.valid_cnt = 0;
    b.isFree = false;
    std::fill(b.valid.begin(), b.valid.end(), false);
    gcActiveBlk_[streamId] = blkId;
    return blkId;
}


// ---------------- Garbage Collection ----------
bool PageMappingFTL::RunGC() {
    Block* victim = (Block*)gcPolicy_->choose_segment();
    u64 victimId = victim ? victim->id : NOT_ALLOCATED;
    if (victimId == NOT_ALLOCATED) {
        // No victim found, nothing to do
        printf("No victim found for GC\n");
        return false;
    }
    if (victim->valid_cnt == PAGES_PER_BLOCK) {
        // No reclaimable space (all valid). Avoid spinning forever.
        printf("GC victim %llu is full (%zu valid); no space can be reclaimed. Need TRIM/OP.\n",
               (unsigned long long)victimId, victim->valid_cnt);
        return false;
    }
    // Allocate destination (spare) block

    // Migrate live pages
    Block& src = blocks_[victimId];
    int valid_page = 0;
    for (u64 idx = 0; idx < PAGES_PER_BLOCK; ++idx) {
        if (!src.valid[idx]) continue;
        u64 blkId = GetOrAllocateGCActiveBlock(0);
        if (blocks_[blkId].full()) {
            assert(false);
        }
        Block& dest = blocks_[blkId];
        nand_write_pages++;
        u64 oldPpn = src.id * PAGES_PER_BLOCK + idx;
        u64 lpn    = GetLpn(oldPpn);
        assert (lpn != NOT_ALLOCATED);

        // copy to dest
        u64 newPpn = dest.id * PAGES_PER_BLOCK + dest.write_ptr;
        dest.valid[dest.write_ptr] = true;
        ++dest.valid_cnt;
        ++dest.write_ptr;
        if (dest.full()) {
            gcPolicy_->add(&dest);
        }
        ++valid_page;
        SetPpn(lpn, newPpn);
    }
    // Erase source block
    src.reset();
    gcPolicy_->remove(&src);
    assert (src.id == victimId);
    std::fill(src.valid.begin(), src.valid.end(), false);
    freePool_.push_back(victimId);

    // Optionally: reclaim dest block if not fully used and almost empty etc.
    return true;
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
