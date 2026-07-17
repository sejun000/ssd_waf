import re
import csv

WARMUP_TIB = 6
WARMUP_MB = WARMUP_TIB * 1024 * 1024  # in MiB
WARMUP_BYTES = WARMUP_TIB * (1024**4)  # in bytes
BLK_TO_TB = 4096.0 / (1000**4)

# ── Simulator stat files (from plot_age_stddev.py (c) panel) ──
sim_stat_files = {
    'REFlash': 'LOG_GREEDY_COST_BENEFIT_80.stat.log.20260304_111119',
    'Greedy':  'LOG_GREEDY_80.stat.log.20260304_131034',
    'SepBIT':  'LOG_SEPBIT_FIFO.stat.log.20260304_124656',
}

# ── Real hardware CSV files ──
real_csv_files = {
    'REFlash': 'LOG_GREEDY_COST_BENEFIT_10_WARM_20260226_153249.csv',
    'Greedy':  'LOG_GREEDY_80_WARM_20260307_155706.csv',
    'SepBIT':  'LOG_SEPBIT_FIFO_20260226_081216.csv',
}

def parse_sim_stat(stat_file):
    """Parse write_size_to_cache, compacted_blocks, evicted_blocks diff after warmup."""
    first = None
    last = None
    with open(stat_file) as f:
        for line in f:
            if 'compacted_blocks:' not in line or 'evicted_blocks:' not in line:
                continue
            wm = re.search(r'write_size_to_cache:\s*(\d+)', line)
            cm = re.search(r'compacted_blocks:\s*(\d+)', line)
            em = re.search(r'evicted_blocks:\s*(\d+)', line)
            if not (wm and cm and em):
                continue
            w, c, e = int(wm.group(1)), int(cm.group(1)), int(em.group(1))
            if w >= WARMUP_BYTES and first is None:
                first = (w, c, e)
            last = (w, c, e)
    if first and last:
        dw = last[0] - first[0]  # host writes (bytes)
        dc = last[1] - first[1]  # compacted (4K blocks)
        de = last[2] - first[2]  # evicted (4K blocks)
        # buffer-tier writes = host writes + GC writes (in 4K blocks)
        buf_blks = dw // 4096 + dc
        return buf_blks, de
    return None, None

def parse_real_csv(csv_file):
    """Parse cache_write_MB (buffer-tier), evict_victim_blocks (flush) diff after warmup."""
    first = None
    last = None
    with open(csv_file) as f:
        reader = csv.DictReader(f)
        for row in reader:
            host_mb = float(row['host_write_MB'])
            cache_mb = float(row['cache_write_MB'])
            ev_blk = int(row['evict_victim_blocks'])
            if host_mb >= WARMUP_MB and first is None:
                first = (cache_mb, ev_blk)
            last = (cache_mb, ev_blk)
    if first and last:
        d_cache_mb = last[0] - first[0]
        # MiB -> 4K blocks
        buf_blks = int(d_cache_mb * 1024 * 1024 / 4096)
        de = last[1] - first[1]
        return buf_blks, de
    return None, None

print(f"{'Policy':<12} {'':^14} {'Sim (4K blks)':>16} {'Real (4K blks)':>16} {'Sim (TB)':>10} {'Real (TB)':>10} {'Error':>8}")
print("-" * 95)

for label in ['Greedy', 'SepBIT', 'REFlash']:
    sim_buf, sim_flush = parse_sim_stat(sim_stat_files[label])
    real_buf, real_flush = parse_real_csv(real_csv_files[label])

    for metric, sv, rv in [('Buffer-tier', sim_buf, real_buf), ('Flush', sim_flush, real_flush)]:
        if sv is None or rv is None or rv == 0:
            print(f"{label:<12} {metric:<14} {'N/A':>16} {'N/A':>16}")
            continue
        s_tb = sv * BLK_TO_TB
        r_tb = rv * BLK_TO_TB
        err = (sv - rv) / rv * 100
        print(f"{label:<12} {metric:<14} {sv:>16,} {rv:>16,} {s_tb:>10.2f} {r_tb:>10.2f} {err:>+7.1f}%")
    print()
