#!/usr/bin/env python3
"""Per-segs (1..128) mean TEC relative error over 6~14 TiB window.
err[t] = (TEC_t(u+δ) − TEC_(t+δ)(u)) / TEC_(t+δ)(u)
        = (r·F_u_delta + G_u_delta)|_t  vs  (r·F_u + G_u)|_(t+δ)
Plots: mean signed err and mean |err| as bars; raw EWMA values (no extra MA).
"""
import os, re, pandas as pd
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

KEY_RE = re.compile(r"(\w+):?\s+(-?\d+(?:\.\d+)?)")
TIB    = 1024**4
R      = 8.64
PERIODS = [1, 2, 4, 8, 16, 32, 64, 128]
T_LO, T_HI = 6.0, 14.0

def find_stat(p):
    for rtag, r_used in [(864, 8.64), (8, 8.0)]:
        path = f"LOG_GREEDY_COST_BENEFIT_10_GS_us02_ewma_hl1572864_gsv4_segs{p}.stat_pr{rtag}"
        if os.path.exists(path): return path, r_used
    return None, None

def parse(path):
    hw, gu, gud, fu, fud = [], [], [], [], []
    with open(path) as fh:
        for line in fh:
            if not line.startswith("LOG_GREEDY"): continue
            kv = dict(KEY_RE.findall(line))
            try:
                hw .append(int(kv["write_size_to_cache"]) / TIB)
                gu .append(float(kv["G_u"]))
                gud.append(float(kv["G_u_delta"]))
                fu .append(float(kv["F_u"]))
                fud.append(float(kv["F_u_delta"]))
            except (KeyError, ValueError): continue
    return tuple(np.array(a) for a in (hw, gu, gud, fu, fud))

segs_used, mean_err, mae, n_pts, r_used_list = [], [], [], [], []
for p in PERIODS:
    path, r_used = find_stat(p)
    if path is None: continue
    hw, gu, gud, fu, fud = parse(path)
    if len(hw) <= p+1: continue
    tec_pred = r_used*fud + gud
    tec_real = r_used*fu  + gu
    pred = tec_pred[:-p]
    real = tec_real[p:]
    t    = hw[:-p]
    m = (t >= T_LO) & (t <= T_HI) & (np.abs(real) > 1e-6)
    err = (pred[m] - real[m]) / real[m]
    if len(err) < 5: continue
    mean_err.append(np.mean(err))
    mae.append(np.mean(np.abs(err)))
    n_pts.append(len(err))
    segs_used.append(p)
    r_used_list.append(r_used)

print(f"6~14 TiB window, TEC pred err = (TEC_t(u+δ) − TEC_(t+δ)(u)) / TEC_(t+δ)(u)")
print(f"{'segs':>4} {'r':>5} {'n':>5} {'mean':>10} {'mean|e|':>10}")
print("-"*45)
for p, ru, n, me, ma in zip(segs_used, r_used_list, n_pts, mean_err, mae):
    print(f"{p:>4} {ru:>5.2f} {n:>5} {me:>+10.4f} {ma:>10.4f}")

# Plot: 2 bars per segs (signed mean, mean|err|)
fig, ax = plt.subplots(figsize=(11, 5.5))
x = np.arange(len(segs_used))
w = 0.36
ax.bar(x - w/2, mean_err, w, label="mean signed err",  color="#4C72B0")
ax.bar(x + w/2, mae,      w, label="mean |err|",        color="#DD8452")
ax.axhline(0, color="gray", lw=0.6, ls="--")
ax.set_xticks(x)
ax.set_xticklabels([f"{p}\n(r={ru})" for p, ru in zip(segs_used, r_used_list)])
ax.set_xlabel("decision period (segs)", fontsize=11)
ax.set_ylabel("TEC relative err", fontsize=11)
ax.grid(axis="y", alpha=0.3)
ax.legend(loc="upper left", fontsize=10)
ax.set_title(f"r=8.64 (* fallback 8.0): TEC err mean over 6~14 TiB window", fontsize=12)

# annotate
for xi, (me, ma) in enumerate(zip(mean_err, mae)):
    ax.text(xi - w/2, me + (0.05 if me>=0 else -0.15), f"{me:+.2f}", ha="center", fontsize=8, color="#4C72B0")
    ax.text(xi + w/2, ma + 0.05, f"{ma:.2f}", ha="center", fontsize=8, color="#DD8452")

fig.tight_layout()
for ext in ("pdf","png"):
    out = f"gs_v4_tec_err_mean_6to14_r864.{ext}"
    fig.savefig(out, bbox_inches="tight")
    print(f"wrote {out}")
