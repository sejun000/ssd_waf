#!/usr/bin/env python3
"""Time series of v_inferred (RHS/(4D+RHS)) vs v_actual (next compact_avg)."""
import os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

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
                "ts":  int(p[IDX["ts"]]),
                "RHS": float(p[IDX["RHS"]]),
                "v":   float(p[IDX["compact_avg"]]),
                "dec": p[IDX["decision"]],
            })
    return rows

def build_series(rows, D=1):
    ts, v_inf, v_act, is_raise = [], [], [], []
    for i in range(len(rows) - 1):
        r = rows[i]
        if r["RHS"] <= 0: continue
        v_a = rows[i+1]["v"]
        if v_a <= 0 or v_a >= 1: continue
        ts.append(r["ts"])
        v_inf.append(r["RHS"] / (4.0 * D + r["RHS"]))
        v_act.append(v_a)
        is_raise.append(r["dec"] == "RAISE")
    return ts, v_inf, v_act, is_raise

def plot_one(ax, path, label, D=1):
    rows = parse(path)
    if not rows:
        ax.set_title(f"{label}: file missing"); return
    ts, v_inf, v_act, is_raise = build_series(rows, D)
    if not ts:
        ax.set_title(f"{label}: no data"); return
    # x in billions of host writes
    x = [t/1e9 for t in ts]
    ax.plot(x, v_inf, label="v_inf = RHS/(4D+RHS)",
            color="tab:red", lw=0.8, alpha=0.7)
    ax.plot(x, v_act, label="v_act = next compact_avg",
            color="tab:blue", lw=0.8, alpha=0.7)
    raise_x = [x[i] for i in range(len(x)) if is_raise[i]]
    raise_y = [v_inf[i] for i in range(len(x)) if is_raise[i]]
    ax.scatter(raise_x, raise_y, s=2, color="tab:orange",
               label="RAISE rows", alpha=0.6)
    ax.set_xlabel("host write (× 1e9 pages)")
    ax.set_ylabel("v (valid frac)")
    ax.set_title(f"{label}  (D={D}, n={len(ts)})")
    ax.set_ylim(0, 1)
    ax.grid(alpha=0.3)
    ax.legend(loc="best", fontsize=8)

fig, axs = plt.subplots(2, 1, figsize=(11, 7), sharex=False)
plot_one(axs[0],
    "/home/sejun000/ssd_waf/LOG_GREEDY_COST_BENEFIT_10_GS_FINAL_us02_ewma_hl1572864_gsdec864cmpAvgD1_d1_pr864.gsdec.log",
    "r=8.64 hl=1·seg", D=1)
plot_one(axs[1],
    "/home/sejun000/ssd_waf/LOG_GREEDY_COST_BENEFIT_10_GS_FINAL_us02_ewma_hl1572864_gsdec288cmpAvgD1_d1_pr288.gsdec.log",
    "r=2.88 hl=1·seg", D=1)
fig.tight_layout()
fig.savefig("/home/sejun000/ssd_waf/A_v_time_series.pdf")
fig.savefig("/home/sejun000/ssd_waf/A_v_time_series.png", dpi=130)
print("saved A_v_time_series.{pdf,png}")
