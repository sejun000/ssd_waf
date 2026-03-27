#!/usr/bin/env python3
"""Compare periodic ratio sweep results (after 6TiB warmup) with DP optimizer."""
import re

TIB = 1024**4
WARMUP = 6 * TIB
BLK = 4096

def parse_stat_line(line):
    m = {}
    for key in ['write_size_to_cache', 'compacted_blocks', 'evicted_blocks', 'reinsert_blocks']:
        match = re.search(rf'{key}:\s*(\d+)', line)
        if match:
            m[key] = int(match.group(1))
    return m

def get_after_warmup(stat_file, warmup_bytes=WARMUP):
    warmup_line = None
    last_line = None
    with open(stat_file) as f:
        for line in f:
            if 'write_size_to_cache' not in line:
                continue
            vals = parse_stat_line(line)
            if vals.get('write_size_to_cache', 0) < warmup_bytes:
                warmup_line = vals
            last_line = vals
    if warmup_line is None or last_line is None:
        return None
    delta = {}
    for k in warmup_line:
        delta[k] = last_line[k] - warmup_line[k]
    return delta

def parse_dp_file(dp_file):
    result = {}
    with open(dp_file) as f:
        for line in f:
            if 'Host write:' in line:
                result['host_write_tb'] = float(re.search(r'[\d.]+', line).group())
            elif 'Compaction:' in line:
                result['compaction_tb'] = float(re.search(r'[\d.]+', line).group())
            elif 'Eviction:' in line:
                m = re.search(r'Eviction:\s+([\d.]+)\s+\(x([\d.]+)\s+=\s+([\d.]+)\)', line)
                if m:
                    result['eviction_tb'] = float(m.group(1))
                    result['eviction_factor'] = float(m.group(2))
                    result['eviction_weighted_tb'] = float(m.group(3))
            elif 'Total:' in line:
                result['total_tb'] = float(re.search(r'[\d.]+', line).group())
    return result

print("=" * 90)
print("Periodic Ratio Sweep Results (after 6TiB warmup)")
print("=" * 90)
print(f"{'pr':>4} {'host_wr':>10} {'compact':>10} {'evict':>10} {'reinsert':>10} | {'host+C':>10} {'E*r':>10} {'TOTAL':>10}")
print(f"{'':>4} {'(TB)':>10} {'(TB)':>10} {'(TB)':>10} {'(TB)':>10} | {'(TB)':>10} {'(TB)':>10} {'(TB)':>10}")
print("-" * 90)

for r in [2, 4, 6, 8, 10]:
    stat_file = f"stat_pr{r}.0"
    try:
        d = get_after_warmup(stat_file)
    except FileNotFoundError:
        print(f"  pr{r}: stat file not found")
        continue
    if d is None:
        print(f"  pr{r}: not enough data")
        continue

    host_tb = d['write_size_to_cache'] / TIB
    compact_tb = d['compacted_blocks'] * BLK / TIB
    evict_tb = d['evicted_blocks'] * BLK / TIB
    reinsert_tb = d['reinsert_blocks'] * BLK / TIB

    total = host_tb + compact_tb + evict_tb * r

    print(f"{r:>4} {host_tb:>10.3f} {compact_tb:>10.3f} {evict_tb:>10.3f} {reinsert_tb:>10.3f} | {host_tb + compact_tb:>10.3f} {evict_tb * r:>10.3f} {total:>10.3f}")

print()
print("=" * 90)
print("DP Optimizer (cb_11_dwpd2_dp.txt)")
print("=" * 90)
dp = parse_dp_file("cb_11_dwpd2_dp.txt")
if dp:
    print(f"  Host write:  {dp.get('host_write_tb',0):.3f} TB")
    print(f"  Compaction:  {dp.get('compaction_tb',0):.3f} TB")
    print(f"  Eviction:    {dp.get('eviction_tb',0):.3f} TB (x{dp.get('eviction_factor',0):.2f} = {dp.get('eviction_weighted_tb',0):.3f} TB)")
    print(f"  Total:       {dp.get('total_tb',0):.3f} TB")
else:
    print("  dp file not found or parse error")
