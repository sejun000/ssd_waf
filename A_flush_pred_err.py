#!/usr/bin/env python3
"""Flush-side prediction error, K-segment window to handle sparsity.
hyp(t)   = f_frac_pred(t)
act(t+1) = mean(Δevict_cum over next K rows) / seg_blocks   per-host-seg avg valid frac
err_pred / err_prev across RAISE rows only.
"""
import os, statistics

SEG_BLOCKS = 1572864
cols = ("ts segs r waf G_u F_u G_ud F_ud LHS RHS decision cur_util "
        "tgt_before tgt_after hard_limit low_floor comp_cum evict_cum "
        "ex_low_tgt ex_tgt_sat ex_force_flush ex_high_valid "
        "compact_avg flush_avg compact_evt ghost_compact_sum f_frac_pred").split()
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
                "ts":          int(p[IDX["ts"]]),
                "f_frac_pred": float(p[IDX["f_frac_pred"]]),
                "evict_cum":   float(p[IDX["evict_cum"]]),
                "dec":         p[IDX["decision"]],
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

def analyze(label, path, K=4):
    rows = parse(path)
    if not rows:
        print(f"=== {label}: file missing ==="); return
    err_pred, err_prev = [], []
    prev_act = None
    for i in range(len(rows) - K):
        if rows[i]["dec"] != "RAISE": continue
        hyp = rows[i]["f_frac_pred"]
        dev = rows[i+K]["evict_cum"] - rows[i]["evict_cum"]
        act = dev / (K * SEG_BLOCKS)
        if hyp <= 0: continue
        if act <= 0: continue
        err_pred.append((act - hyp) / hyp)
        if prev_act is not None and prev_act > 0:
            err_prev.append((act - prev_act) / prev_act)
        prev_act = act
    print(f"=== {label} (RAISE rows, K={K} seg window) ===")
    stats("err_pred = (act(t+1)-hyp(t))/hyp", err_pred)
    stats("err_prev = (act(t+1)-act(k+1))/act(k+1)", err_prev)
    print()

for K in (1, 4, 8, 16):
    print(f"### Window K={K} ###")
    analyze("r=8.64 hl=1·seg",
        "/home/sejun000/ssd_waf/LOG_GREEDY_COST_BENEFIT_10_GS_FINAL_us02_ewma_hl1572864_gsdec864ffPredD1_d1_pr864.gsdec.log", K=K)
    analyze("r=2.88 hl=1·seg",
        "/home/sejun000/ssd_waf/LOG_GREEDY_COST_BENEFIT_10_GS_FINAL_us02_ewma_hl1572864_gsdec288ffPredD1_d1_pr288.gsdec.log", K=K)
