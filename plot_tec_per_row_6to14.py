#!/usr/bin/env python3
"""Per-segs sweep: average TEC per stat row over 6~14 TiB window.
TEC_per_row_MB = (Δcompacted + r·Δevicted) × 4096 / 1024² / Δrows
where Δ is between first row ≥ 6 TiB and last row.  1 row = 6 GiB host write.
"""
import os, re, pandas as pd
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

KEY_RE = re.compile(r"(\w+):?\s+(-?\d+(?:\.\d+)?)")
TIB    = 1024**4
PERIODS = [1, 2, 4, 8, 16, 32, 64, 128]
T_LO, T_HI = 6.0, 14.0

def find_stat(p):
    for rtag, r_used in [(864, 8.64), (8, 8.0)]:
        path = f"LOG_GREEDY_COST_BENEFIT_10_GS_us02_ewma_hl1572864_gsv4_segs{p}.stat_pr{rtag}"
        if os.path.exists(path): return path, r_used
    return None, None

def parse_cum(path):
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
    return rows

# CSAL baseline per-row TEC (also for normalization)
csv = pd.read_csv("ftl0_20260225_082514.csv")
csv["hw_TiB"] = csv["host_write_MB"] / 1024**2
i_lo = (csv["hw_TiB"] - T_LO).abs().idxmin()
i_hi = (csv["hw_TiB"] - T_HI).abs().idxmin()
csal_back = csv.loc[i_hi, "backend_write_MB"] - csv.loc[i_lo, "backend_write_MB"]
csal_hw   = csv.loc[i_hi, "host_write_MB"]    - csv.loc[i_lo, "host_write_MB"]
# 1 row = 6 GiB = 6144 MB host write
csal_rows = csal_hw / 6144.0
print(f"CSAL: back_delta={csal_back:,.0f} MB, hw_delta={csal_hw:,.0f} MB ({csal_rows:.1f} rows)")
print()

segs_used, comp_per_row, evict_per_row, tec_per_row_864, r_used_list = [], [], [], [], []
for p in PERIODS:
    path, r_used = find_stat(p)
    if path is None: continue
    rows = parse_cum(path)
    if not rows: continue
    six_b = T_LO * TIB; fourteen_b = T_HI * TIB
    idx_lo = next((i for i,(hw,_,_) in enumerate(rows) if hw >= six_b), None)
    idx_hi = next((i for i,(hw,_,_) in enumerate(rows) if hw >= fourteen_b), None) or (len(rows)-1)
    if idx_lo is None or idx_lo >= idx_hi: continue
    hw0,c0,e0 = rows[idx_lo]; hw1,c1,e1 = rows[idx_hi]
    n_rows = idx_hi - idx_lo
    comp_mb_total = (c1-c0) * 4096 / 1024**2
    evict_mb_total= (e1-e0) * 4096 / 1024**2
    comp_pr   = comp_mb_total  / n_rows
    evict_pr  = evict_mb_total / n_rows
    # always use r=8.64 for normalized TEC comparison
    tec_pr_864 = comp_pr + 8.64 * evict_pr

    comp_per_row.append(comp_pr)
    evict_per_row.append(evict_pr)
    tec_per_row_864.append(tec_pr_864)
    segs_used.append(p)
    r_used_list.append(r_used)

# CSAL per row (r=8.64, flush only)
csal_tec_pr = 8.64 * csal_back / csal_rows

print(f"6~14 TiB window, per-row averages (1 row ≈ 6 GiB host write)")
print(f"{'segs':>4} {'r_sim':>5} {'rows':>5} {'comp_MB/row':>12} {'evict_MB/row':>13} {'r=8.64 TEC_MB/row':>20} {'vs CSAL_pr':>11}")
print("-"*82)
print(f"{'CSAL':>4} {'-':>5} {csal_rows:>5.0f} {'0':>12} {csal_back/csal_rows:>13.1f} {csal_tec_pr:>20.1f} {'1.000':>11}")
for p, ru, cp, ep, tp in zip(segs_used, r_used_list, comp_per_row, evict_per_row, tec_per_row_864):
    n_rows = "?"
    # recover n_rows
    print(f"{p:>4} {ru:>5.2f} {' ':>5} {cp:>12.1f} {ep:>13.1f} {tp:>20.1f} {tp/csal_tec_pr:>11.3f}")

# bar chart
fig, ax = plt.subplots(figsize=(11, 5.5))
x = np.arange(len(segs_used) + 1)
labels = ["CSAL"] + [f"s{p}" for p in segs_used]
comp_arr  = [0.0] + comp_per_row
flush_arr = [8.64 * csal_back/csal_rows] + [8.64*e for e in evict_per_row]
ax.bar(x, comp_arr, color="#4C72B0", label="comp (GC)")
ax.bar(x, flush_arr, bottom=comp_arr, color="#DD8452", label="r·flush (r=8.64)")
ax.set_xticks(x); ax.set_xticklabels(labels)
ax.set_ylabel("MB per stat row  (1 row = 6 GiB host write)", fontsize=11)
ax.set_title("6~14 TiB: TEC per row (r=8.64), per-segs sweep", fontsize=12)
ax.grid(axis="y", alpha=0.3)
ax.legend(loc="upper right")
for xi, v in enumerate([csal_tec_pr] + tec_per_row_864):
    ax.text(xi, v + 20, f"{v:.0f}", ha="center", fontsize=9)
fig.tight_layout()
for ext in ("pdf","png"):
    out = f"gs_v4_tec_per_row_6to14.{ext}"
    fig.savefig(out, bbox_inches="tight"); print(f"wrote {out}")
