#!/usr/bin/env python3
"""Verify ghost decision rule actually drives target/compaction in the log.

For each seg = N, sample decision-firing rows (multiples of N):
  1. Predicted decision (from G_u, F_u, G_uδ, F_uδ):
       raise  iff  r·(F_u - F_uδ) > G_uδ - G_u
  2. Observed target_valid_rate change between row i and row i+N:
       Δtarget = target[i+N] - target[i]
       sign(Δtarget) > 0 → row observed raise
       sign(Δtarget) < 0 → row observed lower
       |Δtarget| ≠ util_step_ → clipped (hard_limit or 0.0 boundary)
  3. Observed compaction in next N rows:
       dcomp(i→i+N) > 0 → compaction actually fired
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
                             float(kv["G_u_delta"]),
                             float(kv["F_u_delta"]),
                             int(kv["compacted_blocks"]),
                             int(kv["evicted_blocks"]),
                             float(kv["target_valid_rate"]),
                             float(kv["util_step"])))
            except (KeyError, ValueError): continue
    return np.array(rows, dtype=float)

print(f"{T_LO}~{T_HI} TiB.  Decision-firing rows = multiples of segs.")
print(f"  Predicted: raise iff r·(F_u-F_uδ) > G_uδ-G_u   (else lower).")
print(f"  Observed sign: Δtarget over next N rows (>0 raise, <0 lower, =0 stuck).")
print(f"  Observed compaction: dcomp(i→i+N) > 0 in next N rows.")
print(f"  Clipped = |Δtarget| ≠ util_step  (hard_limit 0.9 cap or 0.0 floor).")
print()
print(f"{'segs':>4} {'n':>5}  |  {'P:raise':>7} {'P:lower':>7}  |  "
      f"{'sign_match%':>11} {'clip%':>6} {'stuck%':>6}  |  "
      f"{'raise+comp%':>11} {'lower+comp%':>11}  |  "
      f"{'low_target%':>11}")
print("-"*120)
for p in PERIODS:
    path, r = find_stat(p)
    if path is None: continue
    a = parse(path)
    if len(a) <= p+1: continue
    hw_b = a[:,0]; Gu = a[:,1]; Fu = a[:,2]; Gud = a[:,3]; Fud = a[:,4]
    comp = a[:,5]; evict = a[:,6]; tvr = a[:,7]; ust = a[:,8]
    t = hw_b / TIB

    starts = np.arange(0, len(a)-p, p)
    ends   = starts + p
    in_win = (t[starts] >= T_LO) & (t[starts] <= T_HI)
    starts = starts[in_win]; ends = ends[in_win]
    if len(starts) < 5: continue

    # predicted decision
    pred_raise = (r * (Fu[starts] - Fud[starts])) > (Gud[starts] - Gu[starts])

    # observed target change
    dtgt = tvr[ends] - tvr[starts]
    obs_raise = dtgt > 0
    obs_lower = dtgt < 0
    obs_stuck = dtgt == 0
    sign_match = (pred_raise == obs_raise) | (~pred_raise & obs_lower)   # match on direction
    # but stuck rows: predicted dir didn't show in target => mismatch unless raise hit hard_limit
    # treat stuck as mismatch
    sign_match_pct = sign_match.mean() * 100

    # clip: |dtgt| deviates from util_step at row i (using ust[starts]) by > 1e-6
    eps = 1e-6
    near_step = np.isclose(np.abs(dtgt), ust[starts], atol=eps)
    clip_pct = (~near_step & ~obs_stuck).mean() * 100
    stuck_pct = obs_stuck.mean() * 100

    # observed compaction in next N rows
    dcomp = comp[ends] - comp[starts]
    raise_and_comp = (pred_raise & (dcomp > 0)).sum()
    lower_and_comp = (~pred_raise & (dcomp > 0)).sum()
    n_r = pred_raise.sum()
    n_l = (~pred_raise).sum()
    raise_comp_pct = 100*raise_and_comp/n_r if n_r else float("nan")
    lower_comp_pct = 100*lower_and_comp/n_l if n_l else float("nan")

    # target < 0.1 (skip-compaction path)
    low_target_pct = (tvr[starts] < 0.1).mean() * 100

    print(f"{p:>4} {len(starts):>5}  |  {int(n_r):>7} {int(n_l):>7}  |  "
          f"{sign_match_pct:>10.1f}% {clip_pct:>5.1f}% {stuck_pct:>5.1f}%  |  "
          f"{raise_comp_pct:>10.1f}% {lower_comp_pct:>10.1f}%  |  "
          f"{low_target_pct:>10.1f}%")
