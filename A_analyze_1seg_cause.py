#!/usr/bin/env python3
"""Why does 1 seg over-compact?  Use GS_DEC logs to compare per-seg:
  - raise vs lower decision count (and ratio)
  - target_valid_rate oscillation amplitude (per-row Δtarget std)
  - hard_limit / low_floor hit counts
  - ex_low_tgt, ex_tgt_sat, ex_force_flush, ex_high_valid (final cumulative)
Window: 0~14 TiB host write.
"""
import os, numpy as np

PERIODS = [1, 2, 4, 8]   # GS_DEC logs ran for these
PATH    = "LOG_GREEDY_COST_BENEFIT_10_GS_us02_ewma_hl1572864_gsdec864_segs{p}_pr864.gsdec.log"
TIB     = 1024**4
BLK     = 4096
T_LO, T_HI = 0.0, 14.0

def load(p):
    path = PATH.format(p=p)
    if not os.path.exists(path): return None
    rows = []
    with open(path) as fh:
        hdr = fh.readline().split()
        for line in fh:
            parts = line.split()
            if len(parts) < len(hdr): continue
            d = {}
            for k, v in zip(hdr, parts):
                d[k] = v
            rows.append(d)
    return rows, hdr

print(f"{'segs':>4}  {'rows':>4}  {'raise':>5}  {'lower':>5}  {'%raise':>7}  "
      f"{'|Δtgt|_mean':>11}  {'|Δtgt|_std':>10}  {'hard%':>6}  {'low%':>5}  "
      f"{'ex_force_ff':>11}  {'ex_force/comp':>14}")
print("-"*120)
for p in PERIODS:
    out = load(p)
    if out is None: print(f"{p:>4}  missing"); continue
    rows, hdr = out
    ts   = np.array([int(r["ts"])   for r in rows], dtype=np.int64)
    decs = [r["decision"] for r in rows]
    tgt_b = np.array([float(r["tgt_before"]) for r in rows])
    tgt_a = np.array([float(r["tgt_after"])  for r in rows])
    hard  = np.array([int(r["hard_limit"])   for r in rows])
    low   = np.array([int(r["low_floor"])    for r in rows])
    comp  = np.array([int(r["comp_cum"])     for r in rows], dtype=np.int64)
    ex_ff = np.array([int(r["ex_force_flush"]) for r in rows], dtype=np.int64)

    # row's actual host write at decision time = ts * blk_per_block? ts is in blocks.
    t_tib = ts * BLK / TIB
    m = (t_tib >= T_LO) & (t_tib <= T_HI)
    if m.sum() < 5: print(f"{p:>4}  too few"); continue
    decs_arr = np.array(decs)
    n_r = (decs_arr[m] == "RAISE").sum()
    n_l = (decs_arr[m] == "LOWER").sum()
    pct_r = 100*n_r/m.sum()
    dtg = np.abs(tgt_a[m] - tgt_b[m])
    hard_pct = 100*hard[m].sum() / m.sum()
    low_pct  = 100*low[m].sum()  / m.sum()
    # ex_force_flush is cumulative across whole run; take last value
    ex_force_total = int(ex_ff[m][-1] - ex_ff[m][0]) if m.any() else 0
    comp_total     = int(comp[m][-1] - comp[m][0])  if m.any() else 0
    ex_force_ratio = ex_force_total / max(comp_total, 1)

    print(f"{p:>4}  {int(m.sum()):>4}  {int(n_r):>5}  {int(n_l):>5}  {pct_r:>6.1f}%  "
          f"{dtg.mean():>11.5f}  {dtg.std():>10.5f}  {hard_pct:>5.1f}%  {low_pct:>4.1f}%  "
          f"{ex_force_total:>11d}  {ex_force_ratio:>14.4f}")
