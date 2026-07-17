#!/usr/bin/env python3
"""Same TEC level error analysis as calc_tec_level_err.py, but report ratios
aggregated over short 16-row sub-windows (≈96 GiB each).
For each non-overlapping 16-row block inside 6~14 TiB:
  ratio_block = Σ pred_add / Σ real_add
Then average ratios across blocks (and report median + std).
"""
import os, re
import numpy as np

KEY_RE = re.compile(r"(\w+):?\s+(-?\d+(?:\.\d+)?)")
TIB    = 1024**4
BLK    = 4096
PERIODS = [1, 2, 4, 8, 16, 32, 64, 128]
T_LO, T_HI = 6.0, 14.0
SUBWIN = 16

def find_stat(p):
    for rtag, r in [(864, 8.64), (8, 8.0)]:
        path = f"LOG_GREEDY_COST_BENEFIT_10_GS_us02_ewma_hl1572864_gsv4_segs{p}.stat_pr{rtag}"
        if os.path.exists(path) and os.path.getsize(path) > 0: return path, r
    return None, None

def parse(path):
    rows = []
    with open(path) as fh:
        for line in fh:
            if not line.startswith("LOG_GREEDY"): continue
            kv = dict(KEY_RE.findall(line))
            try:
                rows.append((int(kv["write_size_to_cache"]),
                             int(kv["compacted_blocks"]),
                             int(kv["evicted_blocks"]),
                             float(kv["G_u_delta"]),
                             float(kv["F_u_delta"])))
            except (KeyError, ValueError): continue
    return np.array(rows, dtype=float)

print(f"6~14 TiB, sub-window={SUBWIN} rows (~{SUBWIN*6} GiB), δ = segs (prediction horizon)")
print(f"For each block: ratio = Σpred_add / Σreal_add. Report block-ratio mean/median/std.")
print()
print(f"{'segs':>4} {'r':>5} {'blocks':>6}  |  {'ratio_mean':>10} {'ratio_med':>10} {'ratio_std':>10}  |  {'global_ratio':>12}")
print("-"*84)
for p in PERIODS:
    path, r = find_stat(p)
    if path is None: continue
    a = parse(path)
    if len(a) <= p+1: continue
    hw_b = a[:,0]; comp = a[:,1]; evict = a[:,2]
    Gud  = a[:,3]; Fud  = a[:,4]
    H    = hw_b / BLK
    t    = hw_b / TIB

    dH       = H[p:] - H[:-p]
    pred_add = dH * (1.0 + Gud[:-p] + r*Fud[:-p])
    real_add = (comp[p:] - comp[:-p]) + r*(evict[p:] - evict[:-p]) + dH
    tt       = t[:-p]

    mask = (tt >= T_LO) & (tt <= T_HI) & (real_add > 0)
    idxs = np.where(mask)[0]
    if len(idxs) < SUBWIN: continue

    # non-overlapping 16-row blocks
    ratios = []
    for s in range(0, len(idxs) - SUBWIN + 1, SUBWIN):
        sl = idxs[s:s+SUBWIN]
        rp = pred_add[sl].sum()
        rr = real_add[sl].sum()
        if rr > 0:
            ratios.append(rp / rr)
    ratios = np.array(ratios)
    global_ratio = pred_add[mask].sum() / real_add[mask].sum()
    print(f"{p:>4} {r:>5.2f} {len(ratios):>6}  |  {ratios.mean():>10.4f} {np.median(ratios):>10.4f} {ratios.std():>10.4f}  |  {global_ratio:>12.4f}")
