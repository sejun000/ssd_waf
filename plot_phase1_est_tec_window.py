#!/usr/bin/env python3
"""Window-wise cand est TEC/row over time for each Phase-1 pm sweep.
Reconstruct (g_pmean, f_pmean) per stat-row interval from cumulative sums.
"""
import os, re
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

KEY_RE = re.compile(r"(\w+):?\s+(-?\d+(?:\.\d+)?)")
TIB    = 1024**4
R      = 8.64
ROW_MB = 6144
LIVE   = [1, 2, 4, 8]

def parse(path):
    rows = []
    with open(path) as fh:
        for line in fh:
            if not line.startswith("LOG_GREEDY"): continue
            kv = dict(KEY_RE.findall(line))
            try:
                hw = int(kv["write_size_to_cache"]) / TIB
                row = [hw]
                for k in range(3):
                    row += [int(kv[f"cand{k}_segs"]),
                            int(kv[f"cand{k}_g_n"]),
                            float(kv[f"cand{k}_g_pmean"]),
                            float(kv[f"cand{k}_f_pmean"])]
                rows.append(row)
            except (KeyError, ValueError): continue
    return np.array(rows)

def window_mean(n_arr, pm_arr):
    cum = pm_arr * n_arr     # cumulative raw sum
    win = np.full_like(pm_arr, np.nan, dtype=float)
    for i in range(1, len(n_arr)):
        dn = n_arr[i] - n_arr[i-1]
        dc = cum[i]   - cum[i-1]
        if dn > 0: win[i] = dc / dn
    return win

fig, axes = plt.subplots(2, 2, figsize=(13, 9), sharex=True)
for ax, live in zip(axes.ravel(), LIVE):
    path = f"LOG_GREEDY_COST_BENEFIT_10_GS_us02_ewma_hl1572864_gsv4p1pm_segs{live}.stat_pr864"
    if not os.path.exists(path):
        ax.text(0.5,0.5,"missing",transform=ax.transAxes); continue
    a = parse(path)
    hw = a[:,0]
    colors = ["#4C72B0","#DD8452","#55A868"]
    for k in range(3):
        seg_k = int(a[0, 1+4*k])
        n_arr = a[:, 2+4*k]
        gp    = a[:, 3+4*k]
        fp    = a[:, 4+4*k]
        gp_w  = window_mean(n_arr, gp)
        fp_w  = window_mean(n_arr, fp)
        est_w = (gp_w + R*fp_w) * ROW_MB
        ax.plot(hw, est_w, color=colors[k], lw=1.3, alpha=0.85,
                label=f"cand{k}  δ={seg_k}")
    ax.set_title(f"live segs={live}  (est TEC/row, window)", fontsize=11)
    ax.set_ylabel("est TEC/row (MB)", fontsize=10)
    ax.grid(alpha=0.3)
    ax.legend(loc="upper right", fontsize=9)
for ax in axes[-1]:
    ax.set_xlabel("host write t (TiB)", fontsize=10)
fig.suptitle(f"Phase-1 window-wise cand est TEC/row  (r={R})", fontsize=13)
fig.tight_layout()
for ext in ("pdf","png"):
    out = f"phase1_est_tec_window.{ext}"
    fig.savefig(out, bbox_inches="tight"); print(f"wrote {out}")
