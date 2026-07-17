#!/usr/bin/env python3
"""Ghost rate prediction accuracy (rate-only, no levels, no host write).

For each row t with δ = segs rows ahead:
  pred(t)         = G_u_delta(t)  + r · F_u_delta(t)        # ghost-predicted rate at u+δ
  real(t+δ)       = G_u(t+δ)      + r · F_u(t+δ)            # actual EWMA rate at t+δ
  err(t)          = (pred - real) / real

Window: 0~14 TiB host write.
"""
import os, re
import numpy as np

KEY_RE = re.compile(r"(\w+):?\s+(-?\d+(?:\.\d+)?)")
TIB    = 1024**4
PERIODS = [1, 2, 4, 8, 16, 32, 64, 128]
T_LO, T_HI = 0.0, 14.0
W = 4   # fixed comparison horizon (rows)

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
                             float(kv["G_u"]),
                             float(kv["F_u"]),
                             float(kv["G_u_delta"]),
                             float(kv["F_u_delta"]),
                             int(kv["compacted_blocks"]),
                             int(kv["evicted_blocks"])))
            except (KeyError, ValueError): continue
    return np.array(rows, dtype=float)

print(f"{T_LO}~{T_HI} TiB.  TEC blocks accumulated over fixed W={W}-row window.")
print(f"pred(t) = (G_uδ + r·F_uδ)(t) · ΔH(t→t+W)        # ghost-predicted W-row TEC blocks")
print(f"real(t) = Δcomp(t→t+W) + r·Δevict(t→t+W)        # actual W-row TEC blocks")
print(f"mask: real > 0  AND  Δcomp > 0    Σp/Σr aggregates blocks over ALL masked rows in 0~14 TiB.")
print()
print(f"{'segs':>4} {'r':>5} {'n':>5}  |  {'err_mean':>9} {'|err|':>8} {'err_std':>8}  |  {'Σp/Σr':>8}  {'pred_avg':>9} {'real_avg':>9}")
print("-"*98)
for p in PERIODS:
    path, r = find_stat(p)
    if path is None: continue
    a = parse(path)
    if len(a) <= W+1: continue
    hw_b = a[:,0]; Gu = a[:,1]; Fu = a[:,2]; Gud = a[:,3]; Fud = a[:,4]; comp = a[:,5]; evict = a[:,6]
    H = hw_b / 4096        # host write in blocks
    t = hw_b / TIB

    dH     = H[W:] - H[:-W]                              # ΔH over W rows (blocks)
    dcomp  = comp[W:] - comp[:-W]                        # actual compaction blocks over W rows
    devict = evict[W:] - evict[:-W]                      # actual eviction blocks over W rows

    pred = (Gud[:-W] + r * Fud[:-W]) * dH                # ghost-predicted TEC blocks over next W rows
    real = dcomp + r * devict                            # actual TEC blocks over W rows
    tt   = t[:-W]

    mask = (tt >= T_LO) & (tt <= T_HI) & (real > 0) & (dcomp > 0)
    if mask.sum() < 5: continue
    err = (pred[mask] - real[mask]) / real[mask]
    ratio_sum = pred[mask].sum() / real[mask].sum()
    print(f"{p:>4} {r:>5.2f} {int(mask.sum()):>5}  |  {err.mean():>+9.4f} {np.abs(err).mean():>8.4f} {err.std():>8.4f}  |  {ratio_sum:>8.4f}  {pred[mask].mean():>9.4f} {real[mask].mean():>9.4f}")
