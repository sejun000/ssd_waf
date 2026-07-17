#!/usr/bin/env python3
"""TEC + BUtil 비교.

TEC   = host TB + compaction TB + r × eviction TB   (전체 NAND endurance)
BUtil = global_valid_blocks / total_cache_blocks   (cache valid 비율)
"""
import os, re

SEGS = [1, 2, 4, 8, 16]
SETS = [
    ("buggy r=8.64", 8.64,
     "LOG_GREEDY_COST_BENEFIT_10_GS_us02_ewma_hl1572864_gsdec864_segs{s}.stat_pr864"),
    ("fixed r=8.64", 8.64,
     "LOG_GREEDY_COST_BENEFIT_10_GS_us02_ewma_hl1572864_gsdec864fix_segs{s}.stat_pr864"),
    ("fixed r=2.88", 2.88,
     "LOG_GREEDY_COST_BENEFIT_10_GS_us02_ewma_hl1572864_gsdec288fix_segs{s}.stat_pr288"),
]
KEYS = ("write_size_to_cache", "global_valid_blocks", "total_cache_size",
        "evicted_blocks", "compacted_blocks", "gc_victim_count")
BLK = 4096

def parse_last(path):
    if not os.path.exists(path): return None
    last = None
    with open(path) as f:
        for line in f:
            if "LOG_GREEDY_COST_BENEFIT" in line and "write_size_to_cache" in line:
                last = line
    if last is None: return None
    d = {}
    for k in KEYS:
        m = re.search(rf"{k}:?\s+(\d+)", last)
        if m: d[k] = int(m.group(1))
    return d

for label, r, fmt in SETS:
    print(f"\n=== {label}  (r used in TEC weight = {r}) ===")
    hdr = f"{'seg':>3} | {'host TB':>8} {'comp TB':>8} {'evict TB':>8} {'r·evict':>9} | {'TEC TB':>9} | {'BUtil':>6} {'GC vic':>7}"
    print(hdr); print("-"*len(hdr))
    for s in SEGS:
        d = parse_last(fmt.format(s=s))
        if d is None:
            print(f"{s:>3}  [missing]"); continue
        host  = d["write_size_to_cache"] / 1e12
        comp  = d["compacted_blocks"]    * BLK / 1e12
        evict = d["evicted_blocks"]      * BLK / 1e12
        tec   = host + comp + r * evict
        valid_blocks = d["global_valid_blocks"]
        tot_blocks   = d["total_cache_size"] / BLK
        butil = valid_blocks / tot_blocks if tot_blocks else 0.0
        gc_vic = d["gc_victim_count"]
        print(f"{s:>3} | {host:>8.3f} {comp:>8.3f} {evict:>8.3f} {r*evict:>9.3f} | "
              f"{tec:>9.3f} | {butil:>6.3f} {gc_vic:>7d}")
