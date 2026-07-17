#!/usr/bin/env python3
"""Count rows where G_ud < G_u (RHS = G_ud - G_u < 0).
That means raising util would *reduce* compaction rate per ghost prediction.
"""
import os
SEGS = [1, 2, 4, 8, 16]
FMT = "LOG_GREEDY_COST_BENEFIT_10_GS_us02_ewma_hl1572864_gsdec864_segs{s}_pr864.gsdec.log"

print(f"{'seg':>3} {'n':>5} | {'rhs<0':>6} {'%':>6} | {'rhs<0 & RAISE':>14} {'%':>6} | {'rhs<0 & LOWER':>14} {'%':>6} | {'lhs<0':>6} {'%':>6}")
print("-"*100)
for s in SEGS:
    p = FMT.format(s=s)
    if not os.path.exists(p): continue
    rows = open(p).readlines()[1:]
    n = 0
    n_rhs_neg = 0
    n_rhs_neg_raise = 0
    n_rhs_neg_lower = 0
    n_lhs_neg = 0
    for line in rows:
        t = line.split()
        if len(t) < 11: continue
        Gu  = float(t[4]); Fu = float(t[5]); Gud = float(t[6]); Fud = float(t[7])
        lhs = float(t[8]); rhs = float(t[9])
        dec = t[10]
        n += 1
        if Gud < Gu: n_rhs_neg += 1
        if Gud < Gu and dec == "RAISE": n_rhs_neg_raise += 1
        if Gud < Gu and dec == "LOWER": n_rhs_neg_lower += 1
        if Fu  < Fud: n_lhs_neg += 1
    print(f"{s:>3} {n:>5} | {n_rhs_neg:>6} {100*n_rhs_neg/n:>5.1f}% "
          f"| {n_rhs_neg_raise:>14} {100*n_rhs_neg_raise/max(n_rhs_neg,1):>5.1f}% "
          f"| {n_rhs_neg_lower:>14} {100*n_rhs_neg_lower/max(n_rhs_neg,1):>5.1f}% "
          f"| {n_lhs_neg:>6} {100*n_lhs_neg/n:>5.1f}%")
