#!/usr/bin/env python3
"""Per-seg TEC(t+δ) vs TEC(t) check on seg-aligned offsets.

For each seg = N:
  δ = N rows = one ghost cycle.
  Sample t at multiples of N (t in {0, N, 2N, ...}).
  Mask: dcomp(t→t+δ) > 0  (compaction actually fired in this cycle)
  Compare TEC(t+δ) = G_u(t+δ) + r·F_u(t+δ)  vs  TEC(t) = G_u(t) + r·F_u(t)

Report:
  - n_aligned     : # of aligned samples in 0~14 TiB
  - n_with_dcomp  : how many had dcomp > 0
  - %drop         : fraction of those where TEC(t+δ) < TEC(t)
  - mean ratio    : TEC(t+δ)/TEC(t) mean
  - Σ(t+δ)/Σ(t)  : aggregated ratio
"""
import os, re
import numpy as np

KEY_RE = re.compile(r"(\w+):?\s+(-?\d+(?:\.\d+)?)")
TIB    = 1024**4
PERIODS = [1, 2, 4, 8, 16, 32, 64, 128]
T_LO, T_HI = 0.0, 14.0

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
                             int(kv["compacted_blocks"])))
            except (KeyError, ValueError): continue
    return np.array(rows, dtype=float)

print(f"{T_LO}~{T_HI} TiB.  Seg-aligned sampling (t at multiples of segs), δ = segs rows.")
print(f"TEC(t)   = G_u(t)   + r·F_u(t)        TEC(t+δ) = G_u(t+δ) + r·F_u(t+δ)")
print(f"Cycles where dcomp(t→t+δ) > 0 only.\n")
print(f"{'segs':>4} {'r':>5} {'n_align':>7} {'n_cycle':>7} {'%drop':>7}  |  "
      f"{'mean_ratio':>10} {'med_ratio':>9} {'Σ(t+δ)/Σ(t)':>12}  |  "
      f"{'TEC(t)_avg':>10} {'TEC(t+δ)_avg':>13}")
print("-"*120)
for p in PERIODS:
    path, r = find_stat(p)
    if path is None: continue
    a = parse(path)
    if len(a) <= p+1: continue
    hw_b = a[:,0]; Gu = a[:,1]; Fu = a[:,2]; comp = a[:,3]
    t    = hw_b / TIB

    # aligned start indices: 0, p, 2p, ...
    starts = np.arange(0, len(a) - p, p)
    ends   = starts + p

    tec_t   = Gu[starts] + r * Fu[starts]
    tec_tdl = Gu[ends]   + r * Fu[ends]
    dcomp   = comp[ends] - comp[starts]
    tt      = t[starts]

    mask_t   = (tt >= T_LO) & (tt <= T_HI)
    mask_all = mask_t & (tec_t > 0)
    mask     = mask_all & (dcomp > 0)
    n_align  = int(mask_all.sum())
    n_cycle  = int(mask.sum())
    if n_cycle < 3: continue
    rat   = tec_tdl[mask] / tec_t[mask]
    pdrop = (tec_tdl[mask] < tec_t[mask]).mean() * 100
    sum_r = tec_tdl[mask].sum() / tec_t[mask].sum()
    print(f"{p:>4} {r:>5.2f} {n_align:>7} {n_cycle:>7} {pdrop:>6.1f}%  |  "
          f"{rat.mean():>10.4f} {np.median(rat):>9.4f} {sum_r:>12.4f}  |  "
          f"{tec_t[mask].mean():>10.4f} {tec_tdl[mask].mean():>13.4f}")
