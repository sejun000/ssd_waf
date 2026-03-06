#!/usr/bin/env python3
"""
Compare PERIODIC actual cumulative writes vs DP-predicted cumulative writes.

Periodic: raw cumulative compacted_blocks, evicted_blocks from stat log.
DP prediction: at each step, compute utilization from global_valid_blocks,
look up matching dp.0.XX file's 240GB-smoothed rate at the same write_size
position, accumulate predicted deltas step by step.
"""

import re
import os
import sys
import numpy as np
from collections import defaultdict

# ── Config ──────────────────────────────────────────────────
PERIODIC_LOG = "LOG_GREEDY_COST_BENEFIT_PERIODIC.stat.log.20260306_075203"
DP_DIR       = "cb_11_dwpd1"
WINDOW_GB    = 240.0
BLK_SZ       = 4096
GB           = 1e9
TB           = 1e12
BLKS_TO_GB   = BLK_SZ / GB


# ── Parse stat log (full) ──────────────────────────────────
def parse_stat_log_full(path):
    """Return dict of arrays: ws, compacted, evicted, global_valid, total_cache_size"""
    ws, cb, eb, gv = [], [], [], []
    tcs = None
    with open(path) as f:
        for line in f:
            if 'write_size_to_cache:' not in line:
                continue
            m = {}
            for k in ['write_size_to_cache', 'compacted_blocks', 'evicted_blocks',
                       'global_valid_blocks', 'total_cache_size']:
                found = re.search(rf'{k}:\s+(\d+)', line)
                if found:
                    m[k] = int(found.group(1))
            if len(m) < 4:
                continue
            ws.append(m['write_size_to_cache'])
            cb.append(m['compacted_blocks'])
            eb.append(m['evicted_blocks'])
            gv.append(m['global_valid_blocks'])
            if tcs is None and 'total_cache_size' in m:
                tcs = m['total_cache_size']
    return (np.array(ws, dtype=np.float64),
            np.array(cb, dtype=np.float64),
            np.array(eb, dtype=np.float64),
            np.array(gv, dtype=np.float64),
            tcs)


# ── Parse stat log (minimal, for DP files) ─────────────────
def parse_stat_log_min(path):
    """Return (ws, compacted, evicted) arrays."""
    ws, cb, eb = [], [], []
    with open(path) as f:
        for line in f:
            if 'write_size_to_cache:' not in line:
                continue
            m_ws = re.search(r'write_size_to_cache:\s+(\d+)', line)
            m_cb = re.search(r'compacted_blocks:\s+(\d+)', line)
            m_eb = re.search(r'evicted_blocks:\s+(\d+)', line)
            if m_ws and m_cb and m_eb:
                ws.append(int(m_ws.group(1)))
                cb.append(int(m_cb.group(1)))
                eb.append(int(m_eb.group(1)))
    return (np.array(ws, dtype=np.float64),
            np.array(cb, dtype=np.float64),
            np.array(eb, dtype=np.float64))


# ── Compute 240GB-window smoothed rate for a DP file ───────
def compute_dp_rates(ws, cb, eb, window_bytes):
    """
    For each index i, compute:
      rate_gc  = (cb[i] - cb[j]) / (ws[i] - ws[j]) * BLK_SZ
      rate_fl  = (eb[i] - eb[j]) / (ws[i] - ws[j]) * BLK_SZ
    where j is the index closest to ws[i] - window_bytes.
    Rate = blocks per block written.
    """
    n = len(ws)
    r_gc = np.full(n, np.nan)
    r_fl = np.full(n, np.nan)
    j = 0
    for i in range(n):
        target = ws[i] - window_bytes
        if target <= 0:
            continue
        while j < n - 1 and ws[j + 1] <= target:
            j += 1
        jj = j + 1 if (j + 1 < n and abs(ws[j+1] - target) < abs(ws[j] - target)) else j
        dw = ws[i] - ws[jj]
        if dw <= 0:
            continue
        dw_blks = dw / BLK_SZ
        r_gc[i] = (cb[i] - cb[jj]) / dw_blks  # blocks per block written
        r_fl[i] = (eb[i] - eb[jj]) / dw_blks
    return r_gc, r_fl


# ── DP file path ────────────────────────────────────────────
def dp_path(rate_rounded):
    s = f"{rate_rounded:.2f}".rstrip('0').rstrip('.')
    return os.path.join(DP_DIR, f"dp.{s}")


# ── Interpolate rate at a given ws position ─────────────────
def interp_rate(dp_ws, dp_rate, query_ws):
    """Interpolate dp_rate at query_ws. Returns nan if out of range."""
    valid = ~np.isnan(dp_rate)
    if valid.sum() < 2:
        return np.nan
    return float(np.interp(query_ws, dp_ws[valid], dp_rate[valid],
                           left=np.nan, right=np.nan))


# ── Main ────────────────────────────────────────────────────
def main():
    # 1. Parse periodic stat log
    print(f"Parsing periodic stat log: {PERIODIC_LOG}")
    p_ws, p_cb, p_eb, p_gv, total_cache_bytes = parse_stat_log_full(PERIODIC_LOG)
    total_cache_blks = total_cache_bytes / BLK_SZ
    print(f"  {len(p_ws)} records, WS: {p_ws[0]/TB:.2f}~{p_ws[-1]/TB:.2f} TB")
    print(f"  total_cache_size: {total_cache_bytes/TB:.3f} TB ({total_cache_blks:.0f} blocks)")

    window_bytes = WINDOW_GB * GB

    # 2. Preload all DP files in [0.20 .. 0.88] and compute 240GB rates
    print(f"Loading DP files and computing 240GB rates...")
    dp_rates = {}  # rate_key (e.g. 0.70) → (ws, r_gc, r_fl)
    for r100 in range(20, 89):
        r = r100 / 100.0
        path = dp_path(r)
        if os.path.exists(path):
            d_ws, d_cb, d_eb = parse_stat_log_min(path)
            r_gc, r_fl = compute_dp_rates(d_ws, d_cb, d_eb, window_bytes)
            dp_rates[r100] = (d_ws, r_gc, r_fl)
    print(f"  Loaded {len(dp_rates)} DP files")

    # 3. Walk through periodic step by step, accumulate DP prediction
    n = len(p_ws)
    dp_cum_gc = np.zeros(n)   # accumulated DP-predicted compacted blocks
    dp_cum_fl = np.zeros(n)   # accumulated DP-predicted evicted blocks

    miss_count = 0
    for i in range(1, n):
        # Delta write in this step (in blocks)
        delta_ws_blks = (p_ws[i] - p_ws[i-1]) / BLK_SZ
        if delta_ws_blks <= 0:
            dp_cum_gc[i] = dp_cum_gc[i-1]
            dp_cum_fl[i] = dp_cum_fl[i-1]
            continue

        # Current utilization from global_valid_blocks
        util = p_gv[i] / total_cache_blks
        r100 = int(round(util * 100))
        r100 = max(20, min(88, r100))  # clamp to available range

        if r100 in dp_rates:
            d_ws, r_gc, r_fl = dp_rates[r100]
            rate_gc = interp_rate(d_ws, r_gc, p_ws[i])
            rate_fl = interp_rate(d_ws, r_fl, p_ws[i])
            if np.isnan(rate_gc):
                rate_gc = 0.0
            if np.isnan(rate_fl):
                rate_fl = 0.0
        else:
            rate_gc, rate_fl = 0.0, 0.0
            miss_count += 1

        dp_cum_gc[i] = dp_cum_gc[i-1] + rate_gc * delta_ws_blks
        dp_cum_fl[i] = dp_cum_fl[i-1] + rate_fl * delta_ws_blks

    if miss_count > 0:
        print(f"  WARNING: {miss_count} steps had no matching DP file")

    # 4. Convert to GB for display
    p_gc_gb  = p_cb * BLKS_TO_GB
    p_fl_gb  = p_eb * BLKS_TO_GB
    dp_gc_gb = dp_cum_gc * BLKS_TO_GB
    dp_fl_gb = dp_cum_fl * BLKS_TO_GB

    # 5. Print summary table (every ~600GB)
    step_gb = 600.0
    step_bytes = step_gb * GB
    print(f"\n{'='*120}")
    print(f"Cumulative comparison at {step_gb:.0f}GB intervals  (all values in GB)")
    print(f"{'='*120}")
    hdr = (f"{'WS(TB)':>7} {'Util':>5} "
           f"{'P.GC':>8} {'DP.GC':>8} {'dGC':>8} "
           f"{'P.Flush':>8} {'DP.Flush':>9} {'dFlush':>8} "
           f"{'P.Total':>8} {'DP.Total':>9} {'dTotal':>8} {'dTot%':>7}")
    print(hdr)
    print("-" * 120)

    next_ws = step_bytes
    for i in range(n):
        if p_ws[i] < next_ws:
            continue
        next_ws += step_bytes

        util = p_gv[i] / total_cache_blks
        p_tot = p_gc_gb[i] + p_fl_gb[i]
        d_tot = dp_gc_gb[i] + dp_fl_gb[i]
        diff_gc = p_gc_gb[i] - dp_gc_gb[i]
        diff_fl = p_fl_gb[i] - dp_fl_gb[i]
        diff_tot = p_tot - d_tot
        pct = diff_tot / max(d_tot, 0.01) * 100

        print(f"{p_ws[i]/TB:>7.2f} {util:>5.2f} "
              f"{p_gc_gb[i]:>8.1f} {dp_gc_gb[i]:>8.1f} {diff_gc:>+8.1f} "
              f"{p_fl_gb[i]:>8.1f} {dp_fl_gb[i]:>9.1f} {diff_fl:>+8.1f} "
              f"{p_tot:>8.1f} {d_tot:>9.1f} {diff_tot:>+8.1f} {pct:>+7.1f}")

    # 6. CSV for plotting
    csv_path = "periodic_vs_dp_cumulative.csv"
    with open(csv_path, 'w') as f:
        f.write("ws_tb,util,p_gc_gb,dp_gc_gb,p_flush_gb,dp_flush_gb,"
                "p_total_gb,dp_total_gb\n")
        for i in range(n):
            util = p_gv[i] / total_cache_blks
            f.write(f"{p_ws[i]/TB:.4f},{util:.4f},"
                    f"{p_gc_gb[i]:.2f},{dp_gc_gb[i]:.2f},"
                    f"{p_fl_gb[i]:.2f},{dp_fl_gb[i]:.2f},"
                    f"{p_gc_gb[i]+p_fl_gb[i]:.2f},{dp_gc_gb[i]+dp_fl_gb[i]:.2f}\n")
    print(f"\nCSV: {csv_path}")


if __name__ == '__main__':
    main()
