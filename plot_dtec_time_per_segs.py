#!/usr/bin/env python3
"""Per-segs panels: ΔTEC over time.
  ΔTEC_pred[t] = (G_u_delta + r·F_u_delta) − (G_u + r·F_u)        [rate-units]
  ΔTEC_real[t] = rate[t,t+δ] − rate[t−δ,t]                         [rate-units]
where rate over a δ-row interval = (Δcomp + r·Δevict)/Δhw_blocks.
Smooth with WIN rows for readability.
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
WIN     = 64

def find_stat(p):
    for rtag, r in [(864, 8.64), (8, 8.0)]:
        path = f"LOG_GREEDY_COST_BENEFIT_10_GS_us02_ewma_hl1572864_gsv4_segs{p}.stat_pr{rtag}"
        if os.path.exists(path): return path, r
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
                             int(kv["evicted_blocks"]),
                             float(kv["G_u"]),
                             float(kv["F_u"]),
                             float(kv["G_u_delta"]),
                             float(kv["F_u_delta"])))
            except (KeyError, ValueError): continue
    return np.array(rows, dtype=float)

def box(y, w=WIN):
    if len(y) < w: return y
    return np.convolve(y, np.ones(w)/w, mode="same")

fig, axes = plt.subplots(4, 2, figsize=(14, 14), sharex=True)
for ax, p in zip(axes.ravel(), PERIODS):
    path, r = find_stat(p)
    if path is None:
        ax.text(0.5, 0.5, f"missing segs={p}", ha="center", va="center", transform=ax.transAxes)
        continue
    a = parse(path)
    if len(a) <= 2*p+1:
        ax.text(0.5, 0.5, "not enough rows", ha="center", va="center", transform=ax.transAxes)
        continue
    hw_b = a[:,0]; comp = a[:,1]; evict = a[:,2]
    Gu = a[:,3]; Fu = a[:,4]; Gud = a[:,5]; Fud = a[:,6]
    t  = hw_b / TIB

    # per-row δ-window rate
    rate_back = np.full(len(a), np.nan)
    rate_fwd  = np.full(len(a), np.nan)
    for i in range(p, len(a)-p):
        dh_b = (hw_b[i] - hw_b[i-p]) / BLK
        dh_f = (hw_b[i+p] - hw_b[i]) / BLK
        if dh_b > 0:
            rate_back[i] = ((comp[i]-comp[i-p]) + r*(evict[i]-evict[i-p])) / dh_b
        if dh_f > 0:
            rate_fwd[i]  = ((comp[i+p]-comp[i]) + r*(evict[i+p]-evict[i])) / dh_f

    dtec_pred = (Gud + r*Fud) - (Gu + r*Fu)
    dtec_real = rate_fwd - rate_back

    # restrict 6-14 TiB
    m = (t >= 6) & (t <= 14)
    ax.plot(t[m], box(dtec_pred)[m], color="#4C72B0", lw=1.3,
            label="ΔTEC_pred (ghost: u→u+δ)", alpha=0.9)
    ax.plot(t[m], box(dtec_real)[m], color="#DD8452", lw=1.3,
            label="ΔTEC_real (rate fwd − back)", alpha=0.9)
    ax.axhline(0, color="gray", lw=0.5, ls="--")
    ax.set_title(f"segs={p} (δ={p} rows), r={r}, WIN={WIN}", fontsize=10)
    ax.set_ylabel("ΔTEC rate", fontsize=9)
    ax.grid(alpha=0.3)
    ax.legend(fontsize=8, loc="upper right")

for ax in axes[-1]:
    ax.set_xlabel("host write t (TiB)", fontsize=10)

fig.suptitle(f"v4 EWMA: ΔTEC over time (smoothed WIN={WIN} rows)", fontsize=12)
fig.tight_layout()
for ext in ("pdf","png"):
    out = f"gs_v4_dtec_time_per_segs_w{WIN}.{ext}"
    fig.savefig(out, bbox_inches="tight"); print(f"wrote {out}")
