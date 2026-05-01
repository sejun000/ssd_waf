#!/usr/bin/env python3
"""Compute BIR (Block Invalidation Range) distribution from a blkparse_csv trace.

BIR for a write = #block-write-events between THIS write of LBA X and the NEXT
write of LBA X. Reads are ignored. Multi-block writes are decomposed into
4KB blocks (matching NodapStream::LoadOracle).

Buckets follow Table 2 of the FAST'26 DOGI paper (YCSB-A):
  G1 <200K, G2 200K-9M, G3 9-17M, G4 17-27M, G5 27-42M, G6 42-51M, G7 >51M
"""
import sys
import csv

TRACE = sys.argv[1] if len(sys.argv) > 1 else \
    "/home/sejun000/workloads/logs/cache_io_fio_ycsb_70_20260401_033750.trace"
LIMIT_BLOCKS = int(sys.argv[2]) if len(sys.argv) > 2 else 0  # 0 = full

# 1/10000 of paper boundaries (block-write events).
BOUNDS = [20, 900, 1_700, 2_700, 4_200, 5_100]
LABELS = ["G1<20", "G2<900", "G3<1.7K", "G4<2.7K", "G5<4.2K", "G6<5.1K", "G7>=5.1K"]

def classify(bir):
    for i, b in enumerate(BOUNDS):
        if bir < b:
            return i
    return 6

def main():
    last_ts = {}            # block -> last synthetic_ts
    bucket = [0]*7          # final-classification counter (only counts writes
                            # whose NEXT write was observed inside the window)
    no_invalidation = 0     # writes with no future write (would go to G7=∞)
    synthetic_ts = 0
    total_writes = 0
    total_reads = 0

    with open(TRACE, 'r') as f:
        for line in f:
            parts = line.rstrip().split(',')
            if len(parts) < 4:
                continue
            op = parts[1]
            if op in ('R', 'RS'):
                total_reads += 1
                continue
            if op not in ('W', 'WS'):
                continue
            try:
                off = int(parts[2])
                size = int(parts[3])
            except ValueError:
                continue
            total_writes += 1
            # bytes
            byte_off  = off * 512
            byte_size = size * 512
            block_lo = byte_off // 4096
            block_hi = (byte_off + byte_size + 4095) // 4096  # exclusive
            for blk in range(block_lo, block_hi):
                if blk in last_ts:
                    bir = synthetic_ts - last_ts[blk]
                    bucket[classify(bir)] += 1
                last_ts[blk] = synthetic_ts
                synthetic_ts += 1

            if synthetic_ts % 50_000_000 < block_hi - block_lo:
                print(f"  [{synthetic_ts/1e6:.0f}M events] LBAs={len(last_ts)}",
                      flush=True)
            if LIMIT_BLOCKS and synthetic_ts >= LIMIT_BLOCKS:
                break

    # Blocks that never got a follow-up write fall into G7 conceptually.
    no_invalidation = len(last_ts)
    bucket[6] += no_invalidation

    total = sum(bucket)
    print()
    print(f"trace             : {TRACE}")
    print(f"total_block_writes: {synthetic_ts:,}")
    print(f"trace_writes      : {total_writes:,} (reads skipped: {total_reads:,})")
    print(f"unique_LBAs       : {len(last_ts):,}")
    print(f"no-follow-up      : {no_invalidation:,} (counted in G7)")
    print()
    print(f"{'group':<10} {'count':>14} {'%':>8}")
    print("-"*36)
    for i, lbl in enumerate(LABELS):
        pct = 100*bucket[i]/total if total else 0
        print(f"{lbl:<10} {bucket[i]:>14,} {pct:>7.2f}%")

if __name__ == "__main__":
    main()
