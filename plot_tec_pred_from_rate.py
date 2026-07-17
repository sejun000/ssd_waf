#!/usr/bin/env python3
"""For each v4 EWMA segs sweep:
  real  δ-TEC[t]  = (comp[t+δ] - comp[t]) + r·(evict[t+δ] - evict[t])     [blocks]
  pred  δ-TEC[t]  = (G_u(t) + r·F_u(t)) × Δhost_write_blocks(t→t+δ)         [blocks]
Filter to rows where real Δcomp > 0 (GC actually fired in the δ window).
Plot per-segs scatter / time-series of pred vs real, and print summary error.
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
T_LO, T_HI = 6.0, 14.0   # TiB host-write window
R_864  = 8.64
R_8    = 8.0

def find_stat(p):
    for rtag, r in [(864, R_864), (8, R_8)]:
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
                             float(kv["F_u"])))
            except (KeyError, ValueError): continue
    return np.array(rows, dtype=float)

fig, axes = plt.subplots(4, 2, figsize=(14, 16), sharex=False)
summary = []
for ax, p in zip(axes.ravel(), PERIODS):
    path, r = find_stat(p)
    if path is None:
        ax.text(0.5, 0.5, f"missing segs={p}", ha="center", va="center", transform=ax.transAxes)
        continue
    a = parse(path)
    if len(a) <= p+1:
        ax.text(0.5, 0.5, "too few rows", ha="center", va="center", transform=ax.transAxes)
        continue

    hw_bytes = a[:, 0]
    hw_TiB   = hw_bytes / TIB
    comp     = a[:, 1]
    evict    = a[:, 2]
    Gu       = a[:, 3]
    Fu       = a[:, 4]

    # δ-window deltas
    d_comp_real   = comp[p:]  - comp[:-p]
    d_evict_real  = evict[p:] - evict[:-p]
    d_hw_blocks   = (hw_bytes[p:] - hw_bytes[:-p]) / BLK

    real_tec = d_comp_real + r * d_evict_real
    pred_tec = (Gu[:-p] + r * Fu[:-p]) * d_hw_blocks
    t_TiB    = hw_TiB[:-p]

    # window: 6~14 TiB AND Δcomp > 0
    m = (t_TiB >= T_LO) & (t_TiB <= T_HI) & (d_comp_real > 0)
    n_total = int(((t_TiB >= T_LO) & (t_TiB <= T_HI)).sum())
    n_kept  = int(m.sum())
    if n_kept < 5:
        ax.text(0.5, 0.5, f"only {n_kept} rows w/ Δcomp>0", ha="center", va="center", transform=ax.transAxes)
        summary.append((p, r, n_total, n_kept, np.nan, np.nan, np.nan, np.nan))
        continue

    pr = pred_tec[m]
    rr = real_tec[m]
    err = (pr - rr) / np.where(np.abs(rr) > 1, rr, 1.0)

    # convert blocks → MB for plotting readability
    pr_MB = pr * BLK / 1024**2
    rr_MB = rr * BLK / 1024**2
    ax.plot(t_TiB[m], rr_MB, color="#DD8452", lw=1.2, label="real δ-TEC", alpha=0.85)
    ax.plot(t_TiB[m], pr_MB, color="#4C72B0", lw=1.2, label="pred (G(t)+r·F(t))·Δhw", alpha=0.85)
    ax.set_title(f"segs={p} (δ={p} rows), r={r}, kept={n_kept}/{n_total} (Δcomp>0)", fontsize=10)
    ax.set_ylabel("δ-TEC (MB)", fontsize=9)
    ax.set_xlim(T_LO, T_HI)
    ax.grid(alpha=0.3)
    ax.legend(loc="upper right", fontsize=8)

    summary.append((p, r, n_total, n_kept,
                    float(np.mean(err)), float(np.mean(np.abs(err))),
                    float(np.mean(pr_MB)), float(np.mean(rr_MB))))

for ax in axes[-1]:
    ax.set_xlabel("host write t (TiB)", fontsize=10)
fig.suptitle("v4 EWMA: pred δ-TEC = (G_u(t)+r·F_u(t))·Δhw  vs  real δ-TEC = Δcomp+r·Δevict   [filter: Δcomp>0]", fontsize=12)
fig.tight_layout()
for ext in ("pdf", "png"):
    out = f"gs_v4_pred_from_rate_dcomp_gt0.{ext}"
    fig.savefig(out, bbox_inches="tight"); print(f"wrote {out}")

# table
print()
print(f"{'segs':>4} {'r':>5} {'tot':>5} {'kept':>5} {'mean_e':>10} {'mae':>8} {'pr_MB':>10} {'rr_MB':>10} {'ratio':>8}")
print("-"*78)
for p, r, ntot, nk, me, ma, pm, rm in summary:
    if nk < 5:
        print(f"{p:>4} {r:>5.2f} {ntot:>5} {nk:>5} {'n/a':>10}")
        continue
    print(f"{p:>4} {r:>5.2f} {ntot:>5} {nk:>5} {me:>+10.3f} {ma:>8.3f} {pm:>10.1f} {rm:>10.1f} {pm/rm if rm>0 else 0:>8.3f}")
