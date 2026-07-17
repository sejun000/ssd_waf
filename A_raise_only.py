#!/usr/bin/env python3
"""Sanity check: time-mean of G_u and G_ud across ALL rows vs RAISE-only.
Hypothesis: mean(G_ud) ≈ 4·D · mean(G_u). Prints ratio per D."""
import os

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
            rows.append((float(p[IDX["G_u"]]),
                         float(p[IDX["G_ud"]]),
                         p[IDX["decision"]]))
    return rows

def run(label, base):
    print(f"=== {label} ===")
    print(f"{'D':>3} {'all_Gu':>9} {'all_Gud':>9} {'all_ratio':>10} {'4D':>5}  "
          f"| {'raise_Gu':>9} {'raise_Gud':>10} {'r_ratio':>9}")
    for D in [1,2,4,8,16]:
        p = (f"/home/sejun000/ssd_waf/LOG_GREEDY_COST_BENEFIT_10_GS_FINAL"
             f"_us02_ewma_hl1572864_gsdec{base}finald{D}_d{D}_pr{base}.gsdec.log")
        rows = parse(p)
        if not rows: continue
        gu  = [g for g,_,_ in rows]
        gud = [h for _,h,_ in rows]
        a_gu  = sum(gu)/len(gu)
        a_gud = sum(gud)/len(gud)
        ratio = a_gud/a_gu if a_gu>0 else float("nan")
        r_rows = [(g,h) for g,h,d in rows if d=="RAISE"]
        r_gu  = sum(g for g,_ in r_rows)/len(r_rows) if r_rows else 0
        r_gud = sum(h for _,h in r_rows)/len(r_rows) if r_rows else 0
        r_ratio = r_gud/r_gu if r_gu>0 else float("nan")
        print(f"{D:>3} {a_gu:>9.4f} {a_gud:>9.4f} {ratio:>10.2f} {4*D:>5}  "
              f"| {r_gu:>9.4f} {r_gud:>10.4f} {r_ratio:>9.2f}")
    print()

run("r=8.64 hl=1·seg", "864")
run("r=2.88 hl=1·seg", "288")
