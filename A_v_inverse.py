#!/usr/bin/env python3
"""Invert RHS → v.
RHS ≈ 4D · v/(1-v)  ⟹  v_inferred = RHS / (4D + RHS)
Compare with actual compact_avg of the NEXT row (real GC victim valid frac).
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
                "v":   float(p[IDX["compact_avg"]]),
                "dec": p[IDX["decision"]],
            })
    return rows

def pctl(xs, p):
    if not xs: return float("nan")
    s = sorted(xs); k = max(0, min(len(s)-1, int(p*len(s))))
    return s[k]

def analyze(label, path, D=1):
    rows = parse(path)
    if not rows:
        print(f"{label}: file missing"); return
    v_inf_list, v_act_list, diff_list = [], [], []
    for i in range(len(rows) - 1):
        if rows[i]["dec"] != "RAISE": continue
        rhs = rows[i]["RHS"]
        v_act = rows[i+1]["v"]
        if rhs <= 0: continue
        if v_act <= 0 or v_act >= 1: continue
        v_inf = rhs / (4.0 * D + rhs)
        v_inf_list.append(v_inf)
        v_act_list.append(v_act)
        diff_list.append(v_inf - v_act)
    if not v_inf_list:
        print(f"{label}: no qualifying RAISE rows"); return
    print(f"=== {label}  D={D}  n={len(v_inf_list)} ===")
    print(f"  v_inferred = RHS/(4D+RHS)  mean={statistics.mean(v_inf_list):.4f}  "
          f"median={statistics.median(v_inf_list):.4f}  "
          f"p10={pctl(v_inf_list,0.10):.4f}  p90={pctl(v_inf_list,0.90):.4f}")
    print(f"  v_actual   = next compact_avg mean={statistics.mean(v_act_list):.4f}  "
          f"median={statistics.median(v_act_list):.4f}  "
          f"p10={pctl(v_act_list,0.10):.4f}  p90={pctl(v_act_list,0.90):.4f}")
    print(f"  v_inf - v_act              mean={statistics.mean(diff_list):+.4f}  "
          f"median={statistics.median(diff_list):+.4f}")
    print(f"  ratio  mean(v_inf)/mean(v_act) = "
          f"{statistics.mean(v_inf_list)/statistics.mean(v_act_list):.3f}")
    print()

analyze("r=8.64 hl=1·seg",
        "/home/sejun000/ssd_waf/LOG_GREEDY_COST_BENEFIT_10_GS_FINAL_us02_ewma_hl1572864_gsdec864cmpAvgD1_d1_pr864.gsdec.log", D=1)
analyze("r=2.88 hl=1·seg",
        "/home/sejun000/ssd_waf/LOG_GREEDY_COST_BENEFIT_10_GS_FINAL_us02_ewma_hl1572864_gsdec288cmpAvgD1_d1_pr288.gsdec.log", D=1)
