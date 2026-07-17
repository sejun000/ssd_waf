#!/usr/bin/env python3
"""GC overhead estimate vs RHS prediction.
v = compact_avg (segment-normalized valid frac).
1/(1-v) = avg #GC to free one segment.
v/(1-v) = avg GC-copy overhead to free one segment (in segment units).

For each RAISE row i:
  RHS_i      from row i
  overhead_i from row i+1's compact_avg
Compare mean/median across all RAISE rows.
"""
import os, statistics

cols = ("ts segs r waf G_u F_u G_ud F_ud LHS RHS decision cur_util "
        "tgt_before tgt_after hard_limit low_floor comp_cum evict_cum "
        "ex_low_tgt ex_tgt_sat ex_force_flush ex_high_valid "
        "compact_avg flush_avg compact_evt ghost_compact_sum").split()
IDX = {c: i for i, c in enumerate(cols)}

def parse(path):
    rows = []
    if not os.path.exists(path): return rows
    with open(path) as f:
        for ln in f:
            if ln.startswith("ts"): continue
            p = ln.split()
            if len(p) < len(cols): continue
            rows.append({
                "RHS": float(p[IDX["RHS"]]),
                "compact_avg": float(p[IDX["compact_avg"]]),
                "dec": p[IDX["decision"]],
            })
    return rows

def pctl(xs, p):
    if not xs: return float("nan")
    s = sorted(xs); k = max(0, min(len(s)-1, int(p*len(s))))
    return s[k]

def analyze(label, path):
    rows = parse(path)
    if not rows:
        print(f"{label}: file missing"); return
    rhs_vals = []
    ohd_vals = []
    inv_vals = []  # 1/(1-v) — #GC per segment
    for i in range(len(rows) - 1):
        if rows[i]["dec"] != "RAISE": continue
        rhs = rows[i]["RHS"]
        v = rows[i+1]["compact_avg"]
        if v <= 0.0 or v >= 1.0: continue   # need 0<v<1 for overhead
        ohd = v / (1.0 - v)
        inv = 1.0 / (1.0 - v)
        rhs_vals.append(rhs)
        ohd_vals.append(ohd)
        inv_vals.append(inv)
    if not rhs_vals:
        print(f"{label}: no qualifying RAISE rows"); return
    print(f"=== {label} (n={len(rhs_vals)} RAISE rows w/ valid v_next) ===")
    print(f"  RHS                      mean={statistics.mean(rhs_vals):8.4f}  "
          f"median={statistics.median(rhs_vals):8.4f}  "
          f"p10={pctl(rhs_vals,0.10):8.4f}  p90={pctl(rhs_vals,0.90):8.4f}")
    print(f"  1/(1-v) (#GC/seg)        mean={statistics.mean(inv_vals):8.4f}  "
          f"median={statistics.median(inv_vals):8.4f}  "
          f"p10={pctl(inv_vals,0.10):8.4f}  p90={pctl(inv_vals,0.90):8.4f}")
    print(f"  v/(1-v) (GC overhead)    mean={statistics.mean(ohd_vals):8.4f}  "
          f"median={statistics.median(ohd_vals):8.4f}  "
          f"p10={pctl(ohd_vals,0.10):8.4f}  p90={pctl(ohd_vals,0.90):8.4f}")
    rm = statistics.mean(rhs_vals)
    om = statistics.mean(ohd_vals)
    print(f"  RHS / overhead ratio     mean(RHS)/mean(ohd) = {rm/om if om>0 else float('nan'):.3f}")
    print()

analyze("r=8.64 hl=1·seg D=1",
        "/home/sejun000/ssd_waf/LOG_GREEDY_COST_BENEFIT_10_GS_FINAL_us02_ewma_hl1572864_gsdec864cmpAvgD1_d1_pr864.gsdec.log")
analyze("r=2.88 hl=1·seg D=1",
        "/home/sejun000/ssd_waf/LOG_GREEDY_COST_BENEFIT_10_GS_FINAL_us02_ewma_hl1572864_gsdec288cmpAvgD1_d1_pr288.gsdec.log")
