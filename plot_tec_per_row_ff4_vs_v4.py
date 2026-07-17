#!/usr/bin/env python3
"""6~14 TiB window, per-row TEC (r=8.64) — compare:
  - gsv4   (baseline)            : no force-flush
  - gsv4ff4 (free_pool<=4 force) : force flush when free_pool<=4
Segs 1..64.
"""
import os, re, pandas as pd
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

KEY_RE = re.compile(r"(\w+):?\s+(-?\d+(?:\.\d+)?)")
TIB    = 1024**4
PERIODS = [1, 2, 4, 8, 16, 32, 64]
T_LO, T_HI = 6.0, 14.0
R = 8.64

VARIANTS = [
    ("v4",   "gsv4",    "#4C72B0"),
    ("ff4",  "gsv4ff4", "#DD8452"),
]

def path_for(suffix, p):
    return f"LOG_GREEDY_COST_BENEFIT_10_GS_us02_ewma_hl1572864_{suffix}_segs{p}.stat_pr{864 if R==8.64 else 8}"

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

# CSAL baseline
csv = pd.read_csv("ftl0_20260225_082514.csv")
csv["hw_TiB"] = csv["host_write_MB"] / 1024**2
i_lo = (csv["hw_TiB"] - T_LO).abs().idxmin()
i_hi = (csv["hw_TiB"] - T_HI).abs().idxmin()
csal_back = csv.loc[i_hi, "backend_write_MB"] - csv.loc[i_lo, "backend_write_MB"]
csal_hw   = csv.loc[i_hi, "host_write_MB"]    - csv.loc[i_lo, "host_write_MB"]
csal_rows = csal_hw / 6144.0
csal_tec_pr = R * csal_back / csal_rows
print(f"CSAL: TEC/row(r={R}) = {csal_tec_pr:.1f} MB  ({csal_rows:.1f} rows in window)\n")

results = {}  # results[label] = list of (p, comp_pr, evict_pr, tec_pr)
for label, suffix, color in VARIANTS:
    rows_out = []
    for p in PERIODS:
        f = path_for(suffix, p)
        if not os.path.exists(f):
            rows_out.append((p, None, None, None)); continue
        rows = parse_cum(f)
        if not rows:
            rows_out.append((p, None, None, None)); continue
        six_b = T_LO*TIB; fourteen_b = T_HI*TIB
        idx_lo = next((i for i,(hw,_,_) in enumerate(rows) if hw >= six_b), None)
        idx_hi = next((i for i,(hw,_,_) in enumerate(rows) if hw >= fourteen_b), None) or (len(rows)-1)
        if idx_lo is None or idx_lo >= idx_hi:
            rows_out.append((p, None, None, None)); continue
        hw0,c0,e0 = rows[idx_lo]; hw1,c1,e1 = rows[idx_hi]
        n_rows = idx_hi - idx_lo
        cp = (c1-c0)*4096/1024**2/n_rows
        ep = (e1-e0)*4096/1024**2/n_rows
        tp = cp + R*ep
        rows_out.append((p, cp, ep, tp))
    results[label] = rows_out

# print table
print(f"{'segs':>4} | {'v4 comp':>8} {'v4 evict':>9} {'v4 TEC':>9} {'v4/csal':>8} | {'ff4 comp':>9} {'ff4 evict':>10} {'ff4 TEC':>9} {'ff4/csal':>9} | {'ff4/v4':>7}")
print("-"*110)
for i, p in enumerate(PERIODS):
    v4  = results["v4"][i]
    ff4 = results["ff4"][i]
    def cell(v):
        return tuple("--" if x is None else f"{x:.1f}" for x in v[1:])
    cv = cell(v4); cf = cell(ff4)
    r_v4  = "--" if v4[3]  is None else f"{v4[3]/csal_tec_pr:.3f}"
    r_ff4 = "--" if ff4[3] is None else f"{ff4[3]/csal_tec_pr:.3f}"
    r_diff = "--" if (v4[3] is None or ff4[3] is None) else f"{ff4[3]/v4[3]:.3f}"
    print(f"{p:>4} | {cv[0]:>8} {cv[1]:>9} {cv[2]:>9} {r_v4:>8} | {cf[0]:>9} {cf[1]:>10} {cf[2]:>9} {r_ff4:>9} | {r_diff:>7}")

# bar chart: TEC/row per segs, two variants + CSAL line
fig, ax = plt.subplots(figsize=(11, 5.5))
x = np.arange(len(PERIODS))
w = 0.38
v4_tec  = [results["v4"][i][3]  if results["v4"][i][3]  is not None else np.nan for i in range(len(PERIODS))]
ff4_tec = [results["ff4"][i][3] if results["ff4"][i][3] is not None else np.nan for i in range(len(PERIODS))]
ax.bar(x - w/2, v4_tec,  w, label="v4 baseline",       color="#4C72B0")
ax.bar(x + w/2, ff4_tec, w, label="v4 + ff4 (free≤4 force flush)", color="#DD8452")
ax.axhline(csal_tec_pr, color="gray", lw=1.0, ls="--", label=f"CSAL ({csal_tec_pr:.0f})")
ax.set_xticks(x); ax.set_xticklabels([f"s{p}" for p in PERIODS])
ax.set_ylabel("TEC / row (MB)  [r=8.64]", fontsize=11)
ax.set_xlabel("decision period segs", fontsize=11)
ax.set_title("6~14 TiB per-row TEC: v4 vs v4+ff4 force-flush", fontsize=12)
ax.grid(axis="y", alpha=0.3)
ax.legend(loc="upper left", fontsize=10)
for xi, (a, b) in enumerate(zip(v4_tec, ff4_tec)):
    if not np.isnan(a): ax.text(x[xi]-w/2, a+200, f"{a:.0f}", ha="center", fontsize=8, color="#4C72B0")
    if not np.isnan(b): ax.text(x[xi]+w/2, b+200, f"{b:.0f}", ha="center", fontsize=8, color="#DD8452")
fig.tight_layout()
for ext in ("pdf","png"):
    out = f"gs_v4_ff4_vs_v4_tec_per_row.{ext}"
    fig.savefig(out, bbox_inches="tight"); print(f"wrote {out}")
