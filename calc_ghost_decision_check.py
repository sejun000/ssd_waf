#!/usr/bin/env python3
"""Ghost decision rule correctness check.

For each seg = N:
  Anchor t on rows where compaction OR flush actually happened
  (dcomp_1[i] > 0  OR  devict_1[i] > 0).
  δ = N rows.
  Ghost decision condition (raise target = compact more):
      G_uδ(t) - G_u(t)  <  r · (F_u(t) - F_uδ(t))
      ⇔ TEC_pred(t+δ) < TEC(t)        # ghost says "compaction will lower TEC"

For rows where ghost said "raise" (= compact decision):
  Did actual TEC really drop?   actual TEC(t+δ) < TEC(t) ?

Also for "lower" (ghost said don't compact) — verify symmetric direction.

Window: 0~14 TiB.
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
                             int(kv["evicted_blocks"])))
            except (KeyError, ValueError): continue
    return np.array(rows, dtype=float)

print(f"{T_LO}~{T_HI} TiB.  Anchor t on rows with dcomp_1>0 OR devict_1>0  (event-row align).")
print(f"δ = segs rows.  Ghost says 'raise (compact more)' when  G_uδ-G_u < r·(F_u-F_uδ).")
print()
print(f"{'segs':>4} {'r':>5} {'n_evt':>6} {'n_raise':>7} {'n_lower':>7}  |  "
      f"{'raise→drop%':>11} {'raise_Σtdl/Σt':>14}  |  "
      f"{'lower→drop%':>11} {'lower_Σtdl/Σt':>14}")
print("-"*120)
for p in PERIODS:
    path, r = find_stat(p)
    if path is None: continue
    a = parse(path)
    if len(a) <= p+1: continue
    hw_b = a[:,0]; Gu = a[:,1]; Fu = a[:,2]; Gud = a[:,3]; Fud = a[:,4]
    comp = a[:,5]; evict = a[:,6]
    t = hw_b / TIB

    # 1-row deltas
    dcomp_1  = np.diff(comp,  prepend=comp[0])
    devict_1 = np.diff(evict, prepend=evict[0])

    # event-row anchors: row where compaction or flush actually fired
    event = (dcomp_1 > 0) | (devict_1 > 0)
    starts = np.where(event)[0]
    starts = starts[starts + p < len(a)]
    ends   = starts + p

    tt = t[starts]
    in_win = (tt >= T_LO) & (tt <= T_HI)
    starts = starts[in_win]; ends = ends[in_win]
    if len(starts) < 5: continue

    TEC_t   = Gu[starts] + r * Fu[starts]
    TEC_tdl = Gu[ends]   + r * Fu[ends]
    # ghost decision LHS - RHS:  r*(F_u - F_uδ) - (G_uδ - G_u)  > 0 ⇒ raise (compact)
    decide  = (r * (Fu[starts] - Fud[starts])) - (Gud[starts] - Gu[starts])
    raise_mask = decide > 0
    lower_mask = ~raise_mask

    raise_drop = (TEC_tdl[raise_mask] < TEC_t[raise_mask]).mean() * 100 if raise_mask.any() else float("nan")
    lower_drop = (TEC_tdl[lower_mask] < TEC_t[lower_mask]).mean() * 100 if lower_mask.any() else float("nan")
    raise_sum  = TEC_tdl[raise_mask].sum() / TEC_t[raise_mask].sum() if raise_mask.any() and TEC_t[raise_mask].sum() > 0 else float("nan")
    lower_sum  = TEC_tdl[lower_mask].sum() / TEC_t[lower_mask].sum() if lower_mask.any() and TEC_t[lower_mask].sum() > 0 else float("nan")

    print(f"{p:>4} {r:>5.2f} {len(starts):>6} {int(raise_mask.sum()):>7} {int(lower_mask.sum()):>7}  |  "
          f"{raise_drop:>10.1f}% {raise_sum:>14.4f}  |  "
          f"{lower_drop:>10.1f}% {lower_sum:>14.4f}")
