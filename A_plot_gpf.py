#!/usr/bin/env python3
"""Plot G_u + r*F_u over time (host writes), smoothed by WIN=128/seg per file.
All 5 segs on the same axes for direct comparison.
"""
import os, numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

SEGS = [1, 2, 4, 8, 16]
BASE_WIN = 128
GSDEC_FMT = "LOG_GREEDY_COST_BENEFIT_10_GS_us02_ewma_hl1572864_gsdec864_segs{s}_pr864.gsdec.log"
PAGE_BYTES = 4096

def parse(path):
    ts, r_, Gu, Fu = [], [], [], []
    with open(path) as f:
        next(f)
        for line in f:
            t = line.split()
            if len(t) < 18: continue
            ts.append(int(t[0]))
            r_.append(float(t[2]))
            Gu.append(float(t[4]))
            Fu.append(float(t[5]))
    return np.array(ts), np.array(r_), np.array(Gu), np.array(Fu)

def moving_avg(a, w):
    if w <= 1: return a.copy()
    out = np.full(len(a), np.nan)
    for i in range(len(a)):
        lo = max(0, i - w + 1)
        out[i] = a[lo:i+1].mean()
    return out

fig, ax = plt.subplots(figsize=(11, 5))
colors = {1:"#1f77b4", 2:"#ff7f0e", 4:"#2ca02c", 8:"#d62728", 16:"#9467bd"}
for s in SEGS:
    p = GSDEC_FMT.format(s=s)
    if not os.path.exists(p): continue
    ts, r_, Gu, Fu = parse(p)
    y = Gu + r_ * Fu
    w = max(1, BASE_WIN // s)
    ys = moving_avg(y, w)
    host_tb = ts * PAGE_BYTES / (1024**4)  # TB
    ax.plot(host_tb, ys, color=colors[s], lw=1.2,
            label=f"seg={s} (win={w})")
ax.set_xlabel("host writes (TB)")
ax.set_ylabel(r"$G(u) + r \cdot F(u)$  (smoothed)")
ax.set_title("G_u + r * F_u over time, smoothed by WIN=128/seg")
ax.grid(alpha=0.3)
ax.legend()
fig.tight_layout()
fig.savefig("A_gpf_overlay.png", dpi=120)
fig.savefig("A_gpf_overlay.pdf")
print("wrote A_gpf_overlay.{png,pdf}")
