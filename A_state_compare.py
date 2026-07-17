#!/usr/bin/env python3
"""Compare two error sources on RAISE rows (D=1, hl=1·seg).
  err_pred  = (actual(t+1) - hypothetical(t)) / hypothetical(t)
              ghost prediction error
  err_prev  = (actual(t+1) - actual(prev_raise+1)) / actual(prev_raise+1)
              naive "reuse last RAISE's outcome" error
If |err_prev| ≤ |err_pred|, ghost adds no predictive value.
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

def stats(label, vals):
    if not vals:
        print(f"  {label}: empty"); return
    abs_v = [abs(x) for x in vals]
    print(f"  {label:30s} n={len(vals):5d}  "
          f"signed mean={statistics.mean(vals)*100:+7.2f}%  "
          f"median={statistics.median(vals)*100:+7.2f}%  "
          f"|err| mean={statistics.mean(abs_v)*100:6.2f}%  "
          f"p50={pctl(abs_v,0.5)*100:6.2f}%  "
          f"p90={pctl(abs_v,0.9)*100:6.2f}%")

def analyze(label, path, D=1):
    rows = parse(path)
    if not rows:
        print(f"=== {label}: file missing ==="); return
    err_pred = []
    err_prev = []
    prev_act = None
    for i in range(len(rows) - 1):
        if rows[i]["dec"] != "RAISE": continue
        v_next = rows[i+1]["v"]
        if v_next <= 0 or v_next >= 1: continue
        if rows[i]["RHS"] <= 0: continue
        hyp_t   = rows[i]["RHS"] / (4.0 * D)
        act_tp1 = v_next / (1.0 - v_next)
        # ghost prediction error
        err_pred.append((act_tp1 - hyp_t) / hyp_t)
        # vs previous RAISE's actual
        if prev_act is not None and prev_act > 0:
            err_prev.append((act_tp1 - prev_act) / prev_act)
        prev_act = act_tp1
    print(f"=== {label}  D={D} ===")
    stats("err_pred = (act(t+1) - hyp(t)) / hyp(t)", err_pred)
    stats("err_prev = (act(t+1) - act(k+1)) / act(k+1)", err_prev)
    print()

analyze("r=8.64 hl=1·seg",
        "/home/sejun000/ssd_waf/LOG_GREEDY_COST_BENEFIT_10_GS_FINAL_us02_ewma_hl1572864_gsdec864cmpAvgD1_d1_pr864.gsdec.log")
analyze("r=2.88 hl=1·seg",
        "/home/sejun000/ssd_waf/LOG_GREEDY_COST_BENEFIT_10_GS_FINAL_us02_ewma_hl1572864_gsdec288cmpAvgD1_d1_pr288.gsdec.log")
