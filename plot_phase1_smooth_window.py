#!/usr/bin/env python3
"""Window-wise (incremental) smoothed RMSE of G prediction per candidate.
Reconstructed from cumulative srmse² × sn columns:
    Δsum_sq = (srmse_i²·sn_i) − (srmse_{i-1}²·sn_{i-1})
    Δn      =  sn_i − sn_{i-1}
    win_srmse_i = sqrt(Δsum_sq / Δn)
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
    rows = []
    with open(path) as fh:
        for line in fh:
            if not line.startswith("LOG_GREEDY"): continue
            kv = dict(KEY_RE.findall(line))
            try:
                hw = int(kv["write_size_to_cache"]) / TIB
                row = [hw]
                for k in range(3):
                    row.append(int(kv[f"cand{k}_segs"]))
                    row.append(int(kv[f"cand{k}_g_sn"]))
                    row.append(float(kv[f"cand{k}_g_srmse"]))
                rows.append(row)
            except (KeyError, ValueError):
                continue
    return np.array(rows)

def window_srmse(sn_arr, srmse_arr):
    # cumulative sum of squared err: srmse²·n
    sum_sq = (srmse_arr ** 2) * sn_arr
    win = np.full_like(srmse_arr, np.nan, dtype=float)
    for i in range(1, len(sn_arr)):
        dn  = sn_arr[i]    - sn_arr[i-1]
        dss = sum_sq[i]    - sum_sq[i-1]
        if dn > 0 and dss >= 0:
            win[i] = np.sqrt(dss / dn)
    return win

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
        seg_k = int(a[0, 1+3*k])
        sn    = a[:, 2+3*k]
        sr    = a[:, 3+3*k]
        win   = window_srmse(sn, sr)
        ax.plot(hw, win, color=colors[k], lw=1.3, alpha=0.85,
                label=f"cand{k}  δ={seg_k} segs")
    ax.set_title(f"live segs={p}  (G window srmse, per stat-row)", fontsize=11)
    ax.set_ylabel("window srmse(G)", fontsize=10)
    ax.grid(alpha=0.3)
    ax.legend(loc="upper right", fontsize=9)
for ax in axes[-1]:
    ax.set_xlabel("host write t (TiB)", fontsize=10)
fig.suptitle("Phase-1 window-wise G smoothed-RMSE per candidate δ", fontsize=13)
fig.tight_layout()
for ext in ("pdf","png"):
    out = f"phase1_smooth_g_srmse_window.{ext}"
    fig.savefig(out, bbox_inches="tight")
    print(f"wrote {out}")
