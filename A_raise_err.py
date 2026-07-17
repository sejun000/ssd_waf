#!/usr/bin/env python3
"""D-step RAISE prediction error.
For each D, scan rows. If rows i..i+D-1 are all RAISE,
  pred   = RHS[i]       (= G_ud[i], cum GC prediction at start)
  actual = G_u[i+D]     (D-rows-later cum GC, i.e. one line past the D-th RAISE)
  err_signed = (actual - pred) / pred
  err_abs    = |err_signed|
Signed: + means under-predict, - means over-predict.
"""
import os, statistics

cols = "ts segs r waf G_u F_u G_ud F_ud LHS RHS decision".split()
IDX = {c: i for i, c in enumerate(cols)}

def parse(path):
    rows = []
    if not os.path.exists(path): return rows
    with open(path) as f:
        for ln in f:
            if ln.startswith("ts"): continue
            p = ln.split()
            if len(p) < 11: continue
            rows.append({
                "G_u":  float(p[IDX["G_u"]]),
                "G_ud": float(p[IDX["G_ud"]]),
                "RHS":  float(p[IDX["RHS"]]),
                "dec":  p[IDX["decision"]],
            })
    return rows

def pctl(xs, p):
    if not xs: return float("nan")
    s = sorted(xs); k = max(0, min(len(s)-1, int(p*len(s))))
    return s[k]

def analyze(D, path, label):
    rows = parse(path)
    if not rows:
        print(f"{label} D={D:>2}: file missing"); return
    sgn, abs_e = [], []
    for i in range(len(rows) - D):
        if not all(rows[i+k]["dec"] == "RAISE" for k in range(D)): continue
        pred = rows[i]["RHS"] / (4.0 * D)   # de-scale: ghost rate is ~4D × actual rate
        actual = rows[i+D]["G_u"]
        if pred < 1e-6: continue
        e = (actual - pred) / pred
        sgn.append(e); abs_e.append(abs(e))
    if not sgn:
        print(f"{label} D={D:>2}: no qualifying runs"); return
    print(f"{label} D={D:>2}: n={len(sgn):5d}  "
          f"signed_mean={statistics.mean(sgn)*100:+8.2f}%  "
          f"signed_median={statistics.median(sgn)*100:+8.2f}%  "
          f"|err|_mean={statistics.mean(abs_e)*100:7.2f}%  "
          f"p90={pctl(abs_e,0.9)*100:7.2f}%")

for tag, base in [("r=8.64", "864"), ("r=2.88", "288")]:
    print(f"=== {tag}  hl=1·seg ===")
    for D in [1, 2, 4, 8, 16]:
        path = ("/home/sejun000/ssd_waf/LOG_GREEDY_COST_BENEFIT_10_GS_FINAL"
                f"_us02_ewma_hl1572864_gsdec{base}finald{D}_d{D}_pr{base}.gsdec.log")
        analyze(D, path, tag)
    print()
