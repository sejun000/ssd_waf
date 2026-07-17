#!/usr/bin/env python3
"""Stacked bar: normalized TEC (GC cost + Flush cost) for
CSAL, GS-EWMA (hl=1572864, r=8.64), GS-SMA (w=128segs, r=8.64).
"""
import os, re
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

KEY_RE = re.compile(r"(\w+):?\s+(-?\d+(?:\.\d+)?)")
BLOCK  = 4096
SEGS   = [1, 2, 4, 8, 16, 32, 64, 128]

def parse_tec(path):
    comp = evict = hw_bytes = 0
    with open(path) as fh:
        for line in fh:
            if not line.startswith("LOG_GREEDY"):
                continue
            kv = dict(KEY_RE.findall(line))
            try:
                comp     = int(kv["compacted_blocks"])
                evict    = int(kv["evicted_blocks"])
                hw_bytes = int(kv["write_size_to_cache"])
            except (KeyError, ValueError):
                continue
    hw = hw_bytes / BLOCK
    if hw == 0:
        return None
    return comp / hw, evict / hw

# --- collect data ---
# CSAL: pr8 only available
csal_path = "LOG_GREEDY_COST_BENEFIT_10_GC_us02_ewma_hl1572864.stat_pr8"
csal = parse_tec(csal_path) if os.path.exists(csal_path) else None

ewma_gc, ewma_fl = [], []
sma_gc,  sma_fl  = [], []
valid_segs_ewma, valid_segs_sma = [], []

for p in SEGS:
    # EWMA: prefer pr864, fallback pr8
    for rtag, r_label in [(864, 8.64), (8, 8.0)]:
        path = f"LOG_GREEDY_COST_BENEFIT_10_GS_us02_ewma_hl1572864_gsv4_segs{p}.stat_pr{rtag}"
        if os.path.exists(path):
            res = parse_tec(path)
            if res:
                ewma_gc.append(res[0])
                ewma_fl.append(res[1])
                valid_segs_ewma.append(p)
            break

    # SMA w=201326592 r=864
    path = f"LOG_GREEDY_COST_BENEFIT_10_GS_us02_sma_w201326592_gsv4_segs{p}.stat_pr864"
    if os.path.exists(path):
        res = parse_tec(path)
        if res:
            sma_gc.append(res[0])
            sma_fl.append(res[1])
            valid_segs_sma.append(p)

# --- print table ---
print(f"{'Config':<42} {'GC':>7} {'Flush':>7} {'TEC':>7}")
print("-" * 62)
if csal:
    print(f"{'CSAL (r=8)':<42} {csal[0]:>7.3f} {csal[1]:>7.3f} {csal[0]+csal[1]:>7.3f}")
print()
for p, gc, fl in zip(valid_segs_ewma, ewma_gc, ewma_fl):
    print(f"{'GS-EWMA segs='+str(p):<42} {gc:>7.3f} {fl:>7.3f} {gc+fl:>7.3f}")
print()
for p, gc, fl in zip(valid_segs_sma, sma_gc, sma_fl):
    print(f"{'GS-SMA128 segs='+str(p):<42} {gc:>7.3f} {fl:>7.3f} {gc+fl:>7.3f}")

# --- bar chart ---
n_ewma = len(valid_segs_ewma)
n_sma  = len(valid_segs_sma)

labels = []
gc_vals, fl_vals = [], []

if csal:
    labels.append("CSAL\n(r=8)")
    gc_vals.append(csal[0])
    fl_vals.append(csal[1])

for p, gc, fl in zip(valid_segs_ewma, ewma_gc, ewma_fl):
    labels.append(f"EWMA\ns{p}")
    gc_vals.append(gc)
    fl_vals.append(fl)

for p, gc, fl in zip(valid_segs_sma, sma_gc, sma_fl):
    labels.append(f"SMA128\ns{p}")
    gc_vals.append(gc)
    fl_vals.append(fl)

x       = np.arange(len(labels))
gc_arr  = np.array(gc_vals)
fl_arr  = np.array(fl_vals)

# normalize by CSAL TEC
if csal:
    ref = csal[0] + csal[1]
    gc_arr /= ref
    fl_arr /= ref

fig, ax = plt.subplots(figsize=(max(14, len(labels)*0.9), 6))
bar_gc = ax.bar(x, gc_arr, label="GC Cost",    color="#4C72B0")
bar_fl = ax.bar(x, fl_arr, bottom=gc_arr, label="Flush Cost", color="#DD8452")

ax.set_xticks(x)
ax.set_xticklabels(labels, fontsize=9)
ax.axhline(1.0, color="gray", lw=1.0, ls="--", label="CSAL = 1.0")
ax.set_ylabel("Normalized TEC (/ CSAL r=8)", fontsize=12)
ax.set_title("TEC comparison: CSAL vs GS-EWMA vs GS-SMA(w=128segs)  r=8.64", fontsize=13)
ax.legend(fontsize=10)
ax.grid(axis="y", alpha=0.3)

# value labels
for xi, (gc, fl) in enumerate(zip(gc_arr, fl_arr)):
    total = gc + fl
    ax.text(xi, total + 0.01, f"{total:.2f}", ha="center", va="bottom", fontsize=8)

fig.tight_layout()
for ext in ("pdf", "png"):
    out = f"tec_cmp_sma_ewma.{ext}"
    fig.savefig(out, bbox_inches="tight")
    print(f"wrote {out}")
