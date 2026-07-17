#!/usr/bin/env python3
"""Per-sweep candidate est TEC/row from Phase-1 pmean columns.
est_TEC/row_k = (g_pmean_k + r * f_pmean_k) * 6144 MB
"""
import os, re
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

KEY_RE = re.compile(r"(\w+):?\s+(-?\d+(?:\.\d+)?)")
R      = 8.64
ROW_MB = 6144
LIVE   = [1, 2, 4, 8]

# Collect per-sweep cand triples
rows = []  # (live, cand_seg, est_tec, gp, fp, is_winner_in_sweep)
for live in LIVE:
    path = f"LOG_GREEDY_COST_BENEFIT_10_GS_us02_ewma_hl1572864_gsv4p1pm_segs{live}.stat_pr864"
    if not os.path.exists(path): continue
    with open(path) as fh:
        for line in fh:
            if line.startswith("LOG_GREEDY"): last = line
    kv = dict(KEY_RE.findall(last))
    cands = []
    for k in range(3):
        seg_k = int(kv[f"cand{k}_segs"])
        gp = float(kv[f"cand{k}_g_pmean"])
        fp = float(kv[f"cand{k}_f_pmean"])
        est = (gp + R*fp) * ROW_MB
        cands.append((seg_k, est, gp, fp, k))
    # winner = lowest est
    winner_k = min(cands, key=lambda x: x[1])[4]
    for seg_k, est, gp, fp, k in cands:
        rows.append((live, seg_k, est, gp, fp, k == winner_k))

# Print table
print(f"{'live':>4} {'cand_δ':>7} {'gp':>8} {'fp':>8} {'est_TEC/row MB':>16} {'win':>4}")
print("-"*60)
for live, seg_k, est, gp, fp, win in rows:
    print(f"{live:>4} {seg_k:>7} {gp:>8.4f} {fp:>8.4f} {est:>16.0f} {'*' if win else ''}")

# Plot — grouped bars
fig, ax = plt.subplots(figsize=(11, 5.5))
n_live = len(LIVE)
width = 0.25
x = np.arange(n_live)
colors_cand = ["#4C72B0", "#DD8452", "#55A868"]
for k in range(3):
    vals = [r[2] for r in rows if r[5] is True or (r[5] is False)]  # placeholder
    vals_k = []
    seg_labels = []
    for live in LIVE:
        for live2, seg_k, est, gp, fp, win in rows:
            if live2 == live and (rows.index((live2, seg_k, est, gp, fp, win)) % 3) == k:
                vals_k.append(est); seg_labels.append(seg_k); break
    bar = ax.bar(x + (k-1)*width, vals_k, width, color=colors_cand[k], label=f"cand{k}")
    for xi, (v, sl) in enumerate(zip(vals_k, seg_labels)):
        ax.text(x[xi] + (k-1)*width, v + 200, f"δ={sl}\n{v:.0f}", ha="center", fontsize=8)
# annotate winners
for xi, live in enumerate(LIVE):
    triple = [r for r in rows if r[0] == live]
    win = min(triple, key=lambda r: r[2])
    k = triple.index(win)
    ax.scatter(x[xi] + (k-1)*width, win[2], color="red", marker="v", s=80, zorder=5,
               label="local min" if xi == 0 else None)
ax.set_xticks(x); ax.set_xticklabels([f"live segs={l}" for l in LIVE])
ax.set_ylabel("est TEC / row (MB)  [ghost view, r=8.64]")
ax.set_title("Phase-1 per-cand est TEC/row in each live sweep (ghost-side)", fontsize=12)
ax.legend(loc="upper left", fontsize=9)
ax.grid(axis="y", alpha=0.3)
fig.tight_layout()
for ext in ("pdf","png"):
    out = f"phase1_est_tec_per_row.{ext}"
    fig.savefig(out, bbox_inches="tight"); print(f"wrote {out}")
