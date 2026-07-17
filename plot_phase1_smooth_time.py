#!/usr/bin/env python3
"""Cumulative smoothed RMSE (G prediction) over time for the 3 candidate δ values
within each Phase-1 sweep (segs=1,2,4,8, r=8.64).
"""
import os, re
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

PERIODS = [1, 2, 4, 8]
TAG_FMT = "LOG_GREEDY_COST_BENEFIT_10_GS_us02_ewma_hl1572864_gsv4p1s_segs{p}.stat_pr864"
KEY_RE  = re.compile(r"(\w+):?\s+(-?\d+(?:\.\d+)?)")
TIB     = 1024**4

def parse(path):
    rows = []  # (hw_TiB, cand0_segs, cand0_g_srmse, cand1_segs, cand1_g_srmse, cand2_segs, cand2_g_srmse)
    with open(path) as fh:
        for line in fh:
            if not line.startswith("LOG_GREEDY"): continue
            kv = dict(KEY_RE.findall(line))
            try:
                hw = int(kv["write_size_to_cache"]) / TIB
                row = [hw]
                for k in range(3):
                    row.append(int(kv[f"cand{k}_segs"]))
                    row.append(float(kv[f"cand{k}_g_srmse"]))
                rows.append(row)
            except (KeyError, ValueError):
                continue
    return np.array(rows)

fig, axes = plt.subplots(2, 2, figsize=(13, 9), sharex=True)
for ax, p in zip(axes.ravel(), PERIODS):
    path = TAG_FMT.format(p=p)
    if not os.path.exists(path):
        ax.text(0.5,0.5,f"missing\n{path}",ha="center",va="center",transform=ax.transAxes); continue
    a = parse(path)
    if len(a) == 0:
        ax.text(0.5,0.5,"no data",ha="center",va="center",transform=ax.transAxes); continue
    hw = a[:,0]
    colors = ["#4C72B0","#DD8452","#55A868"]
    for k in range(3):
        seg_k = int(a[0, 1+2*k])
        sr    = a[:, 2+2*k]
        # mask zero (no samples yet)
        ax.plot(hw, np.where(sr>0, sr, np.nan), color=colors[k], lw=1.4,
                label=f"cand{k}  δ={seg_k} segs")
    ax.set_title(f"live segs={p}  (G smoothed RMSE, cumulative)", fontsize=11)
    ax.set_ylabel("srmse(G)", fontsize=10)
    ax.grid(alpha=0.3)
    ax.legend(loc="upper right", fontsize=9)
for ax in axes[-1]:
    ax.set_xlabel("host write t (TiB)", fontsize=10)
fig.suptitle("Phase-1 cumulative G smoothed-RMSE per candidate δ  (SMA 64 segs)", fontsize=13)
fig.tight_layout()
for ext in ("pdf","png"):
    out = f"phase1_smooth_g_srmse_time.{ext}"
    fig.savefig(out, bbox_inches="tight")
    print(f"wrote {out}")
