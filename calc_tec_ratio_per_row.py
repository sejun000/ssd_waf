#!/usr/bin/env python3
"""Per-row pred/real ratio for fixed 8-row window, sweep over segs (ghost δ).
Filter: Δcomp > 0 (compaction actually fired in the next 8 rows).
TEC includes host write:
  pred_add(t) = ΔH(t→t+W) · (1 + G_u_delta(t) + r·F_u_delta(t))   [ghost rate × W rows]
  real_add(t) = (comp[t+W]-comp[t]) + r·(evict[t+W]-evict[t]) + ΔH
Window: 0~14 TiB host write.  W=8 rows fixed.
"""
import os, re
import numpy as np

KEY_RE = re.compile(r"(\w+):?\s+(-?\d+(?:\.\d+)?)")
TIB    = 1024**4
BLK    = 4096
PERIODS = [1, 2, 4, 8, 16, 32, 64, 128]
T_LO, T_HI = 0.0, 14.0
W = 8

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

print(f"{T_LO}~{T_HI} TiB, window = {W} rows fixed (sweep over segs = ghost δ).  NO Δcomp filter (all rows).")
print(f"per-row ratio = pred_add(t) / real_add(t).")
print()
print(f"{'segs':>4} {'r':>5} {'rows':>5}  |  {'mean':>8} {'median':>8} {'std':>8}  |  {'Σp/Σr':>8}")
print("-"*70)
for p in PERIODS:
    path, r = find_stat(p)
    if path is None: continue
    a = parse(path)
    if len(a) <= W+1: continue
    hw_b = a[:,0]; comp = a[:,1]; evict = a[:,2]
    Gud  = a[:,3]; Fud  = a[:,4]
    H    = hw_b / BLK
    t    = hw_b / TIB

    dH       = H[W:] - H[:-W]
    dcomp    = comp[W:] - comp[:-W]
    devict   = evict[W:] - evict[:-W]
    pred_add = dH * (1.0 + Gud[:-W] + r*Fud[:-W])
    real_add = dcomp + r*devict + dH
    tt       = t[:-W]

    mask = (tt >= T_LO) & (tt <= T_HI) & (real_add > 0)
    if mask.sum() < 5: continue
    ratios = pred_add[mask] / real_add[mask]
    global_ratio = pred_add[mask].sum() / real_add[mask].sum()
    print(f"{p:>4} {r:>5.2f} {int(mask.sum()):>5}  |  {ratios.mean():>8.4f} {np.median(ratios):>8.4f} {ratios.std():>8.4f}  |  {global_ratio:>8.4f}")
