#!/usr/bin/env python3
"""Distribution of F_u vs F_ud (LHS sign = sign of F_u - F_ud).
Also break down by decision and by sign of RHS (G_ud - G_u).
"""
import os
import numpy as np
SEGS = [1, 2, 4, 8, 16]
FMT = "LOG_GREEDY_COST_BENEFIT_10_GS_us02_ewma_hl1572864_gsdec864_segs{s}_pr864.gsdec.log"

print(f"{'seg':>3} {'n':>5} | {'F_u>F_ud':>9} {'%':>5} {'F_u==F_ud':>10} {'%':>5} {'F_u<F_ud':>9} {'%':>5} "
      f"| {'F_u mean':>9} {'F_ud mean':>10} {'(F_u-F_ud) mean':>16} {'med':>8} {'p90':>8}")
print("-"*150)
for s in SEGS:
    p = FMT.format(s=s)
    if not os.path.exists(p): continue
    rows = open(p).readlines()[1:]
    Fu_a=[]; Fud_a=[]; gt=0; eq=0; lt=0
    for line in rows:
        t = line.split()
        if len(t) < 11: continue
        Fu = float(t[5]); Fud = float(t[7])
        Fu_a.append(Fu); Fud_a.append(Fud)
        if Fu > Fud: gt += 1
        elif Fu < Fud: lt += 1
        else: eq += 1
    n = len(Fu_a)
    Fu_a = np.array(Fu_a); Fud_a = np.array(Fud_a)
    diff = Fu_a - Fud_a
    print(f"{s:>3} {n:>5} | {gt:>9} {100*gt/n:>4.1f}% {eq:>10} {100*eq/n:>4.1f}% {lt:>9} {100*lt/n:>4.1f}% "
          f"| {Fu_a.mean():>9.4f} {Fud_a.mean():>10.4f} {diff.mean():>16.4f} {np.median(diff):>8.4f} {np.percentile(diff,90):>8.4f}")

print()
print(f"{'seg':>3} | {'RAISE rows':>10} {'F_u-F_ud mean':>14} | {'LOWER rows':>10} {'F_u-F_ud mean':>14}")
print("-"*70)
for s in SEGS:
    p = FMT.format(s=s)
    if not os.path.exists(p): continue
    rows = open(p).readlines()[1:]
    rR=[]; rL=[]
    for line in rows:
        t = line.split()
        if len(t) < 11: continue
        d = float(t[5]) - float(t[7])
        if t[10] == "RAISE": rR.append(d)
        elif t[10] == "LOWER": rL.append(d)
    print(f"{s:>3} | {len(rR):>10} {np.mean(rR) if rR else float('nan'):>14.4f} | {len(rL):>10} {np.mean(rL) if rL else float('nan'):>14.4f}")
