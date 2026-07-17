#!/usr/bin/env python3
"""Time series of GC overhead per free-segment.
Hypothetical state (t)  = RHS / (4D)       (RAISE-rows only)
Actual state (t+1)      = v / (1 - v)      where v = next row's compact_avg
y axis: segment units (>=1 means copy cost exceeds the freed segment)."""
import os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

BASE = 10
plt.rcParams.update({
    "font.size":        BASE * 1.3,
    "axes.labelsize":   BASE * 1.3,
    "xtick.labelsize":  BASE * 1.3,
    "ytick.labelsize":  BASE * 1.3,
    "legend.fontsize":  BASE * 1.3,
})

# 1 page = 4096 B → 1 TiB = 2^40 B = 2^28 pages
PAGES_PER_TIB = float(1 << 28)

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
    ts, ghost_oh, real_oh, is_raise = [], [], [], []
    for i in range(len(rows) - 1):
        r = rows[i]
        v_a = rows[i+1]["v"]
        if r["RHS"] <= 0: continue
        if v_a <= 0 or v_a >= 1: continue
        ts.append(r["ts"])
        ghost_oh.append(r["RHS"] / (4.0 * D))
        real_oh.append(v_a / (1.0 - v_a))
        is_raise.append(r["dec"] == "RAISE")
    return ts, ghost_oh, real_oh, is_raise

def rolling_mean(xs, w):
    from collections import deque
    out, s, q = [], 0.0, deque()
    for x in xs:
        q.append(x); s += x
        if len(q) > w: s -= q.popleft()
        out.append(s / len(q))
    return out

def plot_one(ax, caption, path, D=1, ma=32):
    rows = parse(path)
    if not rows:
        ax.text(0.5, 0.5, f"{caption}: file missing", ha="center"); return None
    ts, gh, rh, is_raise = build_series(rows, D)
    if not ts:
        ax.text(0.5, 0.5, f"{caption}: no data", ha="center"); return None
    x_all = [t/PAGES_PER_TIB for t in ts]
    rh_ma = rolling_mean(rh, ma)
    raise_idx = [i for i in range(len(ts)) if is_raise[i]]
    x_r  = [ts[i]/PAGES_PER_TIB for i in raise_idx]
    gh_r = [gh[i]                for i in raise_idx]
    gh_r_ma = rolling_mean(gh_r, ma)
    h_hyp, = ax.plot(x_r, gh_r_ma,
                     label="Hypothetical state (t)",
                     color="tab:red", lw=1.3)
    h_act, = ax.plot(x_all, rh_ma,
                     label="Actual state (t+1)",
                     color="tab:blue", lw=1.3)
    ax.axhline(1.0, color="gray", lw=0.6, ls="--", alpha=0.5)
    ax.grid(alpha=0.3)
    ax.set_xlabel(caption, labelpad=10)
    return (h_hyp, h_act)

fig, axs = plt.subplots(2, 1, figsize=(11, 7))
handles = plot_one(axs[0], "(a) r=8.64",
    "/home/sejun000/ssd_waf/LOG_GREEDY_COST_BENEFIT_10_GS_FINAL_us02_ewma_hl1572864_gsdec864cmpAvgD1_d1_pr864.gsdec.log")
plot_one(axs[1], "(b) r=2.88",
    "/home/sejun000/ssd_waf/LOG_GREEDY_COST_BENEFIT_10_GS_FINAL_us02_ewma_hl1572864_gsdec288cmpAvgD1_d1_pr288.gsdec.log")

# Bottom-only x-axis label for "Host write (TiB)" — shared via fig.supxlabel
fig.supxlabel("Host write (TiB)", fontsize=BASE*1.3, y=0.02)
fig.supylabel("Copied blocks / Freed segments", fontsize=BASE*1.3, x=0.005)

# Top-center legend outside the axes
if handles:
    fig.legend(handles=list(handles),
               loc="upper center",
               ncol=2,
               bbox_to_anchor=(0.5, 0.99),
               frameon=False)

fig.tight_layout(rect=[0.03, 0.04, 1.0, 0.93])
fig.savefig("/home/sejun000/ssd_waf/A_overhead_time_series.pdf")
fig.savefig("/home/sejun000/ssd_waf/A_overhead_time_series.png", dpi=130)
print("saved A_overhead_time_series.{pdf,png}")
