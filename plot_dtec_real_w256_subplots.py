#!/usr/bin/env python3
"""Per-segs subplots: ΔTEC_real = rate[t, t+W] − rate[t−W, t] over time.
W=256 rows fixed window. 4×2 subplots, one per segs.
"""
import os, re
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

KEY_RE = re.compile(r"(\w+):?\s+(-?\d+(?:\.\d+)?)")
TIB    = 1024**4
BLK    = 4096
PERIODS = [1, 2, 4, 8, 16, 32, 64, 128]
W = 256

def find_stat(p):
    for rtag, r in [(864, 8.64), (8, 8.0)]:
        path = f"LOG_GREEDY_COST_BENEFIT_10_GS_us02_ewma_hl1572864_gsv4_segs{p}.stat_pr{rtag}"
        if os.path.exists(path) and os.path.getsize(path) > 0: return path, r
    return None, None

def parse(path):
    rows = []
    with open(path) as fh:
        for line in fh:
            if not line.startswith("LOG_GREEDY"): continue
            kv = dict(KEY_RE.findall(line))
            try:
                rows.append((int(kv["write_size_to_cache"]),
                             int(kv["compacted_blocks"]),
                             int(kv["evicted_blocks"])))
            except (KeyError, ValueError): continue
    return np.array(rows, dtype=float)

fig, axes = plt.subplots(4, 2, figsize=(14, 14), sharex=True)
colors = plt.cm.viridis(np.linspace(0, 0.92, len(PERIODS)))

for ax, p, c in zip(axes.ravel(), PERIODS, colors):
    path, r = find_stat(p)
    if path is None:
        ax.text(0.5, 0.5, f"missing segs={p}", ha="center", va="center", transform=ax.transAxes)
        continue
    a = parse(path)
    if len(a) <= 2*W+1:
        ax.text(0.5, 0.5, "not enough rows", ha="center", va="center", transform=ax.transAxes)
        continue
    hw_b, comp, evict = a[:,0], a[:,1], a[:,2]
    t = hw_b / TIB
    dtec = np.full(len(a), np.nan)
    for i in range(W, len(a)-W):
        dhb = (hw_b[i] - hw_b[i-W]) / BLK
        dhf = (hw_b[i+W] - hw_b[i]) / BLK
        if dhb > 0 and dhf > 0:
            rate_back = ((comp[i]-comp[i-W]) + r*(evict[i]-evict[i-W])) / dhb
            rate_fwd  = ((comp[i+W]-comp[i]) + r*(evict[i+W]-evict[i])) / dhf
            dtec[i] = rate_fwd - rate_back
    m = (t >= 6) & (t <= 14) & ~np.isnan(dtec)
    ax.plot(t[m], dtec[m], color=c, lw=1.2, alpha=0.9)
    ax.axhline(0, color="gray", lw=0.5, ls="--")
    ax.set_title(f"segs={int(p)} (r={r}, W={W} rows)", fontsize=10)
    ax.set_ylabel("ΔTEC_real", fontsize=9)
    ax.grid(alpha=0.3)

for ax in axes[-1]:
    ax.set_xlabel("host write t (TiB)", fontsize=10)

fig.suptitle(f"v4 EWMA: ΔTEC_real = rate[t,t+{W}] − rate[t−{W},t]  (per-segs subplots, 6~14 TiB)", fontsize=12)
fig.tight_layout()
for ext in ("pdf","png"):
    out = f"gs_v4_dtec_real_w{W}_subplots.{ext}"
    fig.savefig(out, bbox_inches="tight"); print(f"wrote {out}")
