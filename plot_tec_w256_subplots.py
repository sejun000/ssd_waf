#!/usr/bin/env python3
"""Per-segs subplots: TEC over a fixed W=256-row window  (cost in that window only).
TEC[t] = (comp[t+W]-comp[t]) + r·(evict[t+W]-evict[t])              [blocks]
       — purely the cost incurred in the next W rows. Always ≥ 0.
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
W = 32

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
    if len(a) <= W+1:
        ax.text(0.5, 0.5, "not enough rows", ha="center", va="center", transform=ax.transAxes)
        continue
    hw_b, comp, evict = a[:,0], a[:,1], a[:,2]
    t = hw_b / TIB
    tec = np.full(len(a), np.nan)
    for i in range(0, len(a)-W):
        tec[i] = ((comp[i+W]-comp[i]) + r*(evict[i+W]-evict[i])) * BLK / 1024**2   # MB
    m = (t >= 6) & (t <= 14) & ~np.isnan(tec)
    ax.plot(t[m], tec[m], color=c, lw=1.2, alpha=0.9)
    ax.set_title(f"segs={int(p)} (r={r}, W={W} rows ≈ 1.5 TiB)", fontsize=10)
    ax.set_ylabel("TEC over W rows (MB)", fontsize=9)
    ax.grid(alpha=0.3)

for ax in axes[-1]:
    ax.set_xlabel("host write t (TiB)", fontsize=10)

fig.suptitle(f"v4 EWMA: TEC over fixed {W}-row window  (cost in [t, t+{W}], 6~14 TiB)", fontsize=12)
fig.tight_layout()
for ext in ("pdf","png"):
    out = f"gs_v4_tec_w{W}_subplots.{ext}"
    fig.savefig(out, bbox_inches="tight"); print(f"wrote {out}")
