#!/usr/bin/env python3
"""D=1 only: try several de-scale factors (1, 3, 4, 10) and report error stats."""
import os, statistics

PATH = "/home/sejun000/ssd_waf/LOG_GREEDY_COST_BENEFIT_10_GS_FINAL_us02_ewma_hl1572864_gsdec864finald1_d1_pr864.gsdec.log"
D = 1

cols = "ts segs r waf G_u F_u G_ud F_ud LHS RHS decision".split()
IDX = {c: i for i, c in enumerate(cols)}

rows = []
with open(PATH) as f:
    for ln in f:
        if ln.startswith("ts"): continue
        p = ln.split()
        if len(p) < 11: continue
        rows.append({
            "G_u":  float(p[IDX["G_u"]]),
            "RHS":  float(p[IDX["RHS"]]),
            "dec":  p[IDX["decision"]],
        })

def pctl(xs, p):
    if not xs: return float("nan")
    s = sorted(xs); k = max(0, min(len(s)-1, int(p*len(s))))
    return s[k]

print(f"D=1 r=8.64 hl=1·seg, n_rows={len(rows)}")
print(f"{'factor':>8} {'n':>5} {'signed_mean':>14} {'signed_med':>13} {'|err|_mean':>13} {'|err|_p90':>11}")
for factor_name, factor in [("RHS", 1), ("RHS/3D", 3.0*D), ("RHS/4D", 4.0*D), ("RHS/10.2", 10.2)]:
    sgn, abs_e = [], []
    for i in range(len(rows) - D):
        if not all(rows[i+k]["dec"] == "RAISE" for k in range(D)): continue
        pred = rows[i]["RHS"] / factor
        actual = rows[i+D]["G_u"]
        if pred < 1e-6: continue
        e = (actual - pred) / pred
        sgn.append(e); abs_e.append(abs(e))
    if sgn:
        print(f"{factor_name:>8} {len(sgn):>5} "
              f"{statistics.mean(sgn)*100:>13.2f}% {statistics.median(sgn)*100:>12.2f}% "
              f"{statistics.mean(abs_e)*100:>12.2f}% {pctl(abs_e,0.9)*100:>10.2f}%")
