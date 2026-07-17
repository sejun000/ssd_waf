#!/usr/bin/env python3
"""Stacked bar: normalized TEC (GC + r*Flush) for CSAL vs GS-EWMA, r=2.88."""
import os, re
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

KEY_RE = re.compile(r"(\w+):?\s+(-?\d+(?:\.\d+)?)")
BLOCK  = 4096
R      = 2.88
SEGS   = [1, 2, 4, 8, 16, 32]

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
    if hw == 0: return None
    return comp/hw, evict/hw

csal = parse_tec("LOG_GREEDY_COST_BENEFIT_10_GC_us02_ewma_hl1572864.stat_pr8")
csal_tec = csal[0] + R * csal[1]

labels   = ["CSAL\n(r=2.88)"]
gc_vals  = [csal[0]]
fl_vals  = [R * csal[1]]

print(f"{'Config':<30} {'GC':>6} {'r*Flush':>8} {'TEC':>7} {'norm':>6}")
print("-" * 58)
print(f"{'CSAL (r=2.88)':<30} {csal[0]:>6.3f} {R*csal[1]:>8.3f} {csal_tec:>7.3f} {'1.000':>6}")

for p in SEGS:
    path = f"LOG_GREEDY_COST_BENEFIT_10_GS_us02_ewma_hl1572864_gsv4_segs{p}.stat_pr288"
    if not os.path.exists(path): continue
    res = parse_tec(path)
    if res is None: continue
    gc, fl_raw = res
    fl = R * fl_raw
    tec = gc + fl
    norm = tec / csal_tec
    print(f"{'GS-EWMA segs='+str(p):<30} {gc:>6.3f} {fl:>8.3f} {tec:>7.3f} {norm:>6.3f}")
    labels.append(f"GS-EWMA\ns={p}")
    gc_vals.append(gc)
    fl_vals.append(fl)

# normalize by CSAL
ref = csal_tec
gc_arr = np.array(gc_vals) / ref
fl_arr = np.array(fl_vals) / ref

x = np.arange(len(labels))
fig, ax = plt.subplots(figsize=(10, 6))
ax.bar(x, gc_arr, label="GC Cost",    color="#4C72B0")
ax.bar(x, fl_arr, bottom=gc_arr, label=f"r×Flush Cost (r={R})", color="#DD8452")
ax.axhline(1.0, color="gray", lw=1.0, ls="--", label="CSAL = 1.0")

for xi, (gc, fl) in enumerate(zip(gc_arr, fl_arr)):
    ax.text(xi, gc + fl + 0.01, f"{gc+fl:.3f}", ha="center", va="bottom", fontsize=9)

ax.set_xticks(x)
ax.set_xticklabels(labels, fontsize=10)
ax.set_ylabel("Normalized TEC (/ CSAL)", fontsize=12)
ax.set_title(f"TEC: CSAL vs GS-EWMA (hl=1572864)   r={R}", fontsize=13)
ax.legend(fontsize=10)
ax.grid(axis="y", alpha=0.3)
fig.tight_layout()

for ext in ("pdf", "png"):
    out = f"tec_ewma_r288.{ext}"
    fig.savefig(out, bbox_inches="tight")
    print(f"wrote {out}")
