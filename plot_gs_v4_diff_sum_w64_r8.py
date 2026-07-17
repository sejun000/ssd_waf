#!/usr/bin/env python3
"""Per-segs panels of (G(u+δ) − G(u)), (F(u+δ) − F(u)) and their sum over time,
v4 EWMA r=8 sweep, WIN=64 rows box MA on raw series before subtracting.
"""
import os, re
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

PERIODS = [1, 2, 4, 8, 16, 32, 64, 128]
R       = 8
TAG_FMT = "LOG_GREEDY_COST_BENEFIT_10_GS_us02_ewma_hl1572864_gsv4_segs{p}.stat_pr{r}"
KEY_RE  = re.compile(r"(\w+):?\s+(-?\d+(?:\.\d+)?)")
TIB     = 1024**4
WIN     = 64

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

def smooth(y, win=WIN):
    if len(y) < win: return y
    return np.convolve(y, np.ones(win)/win, mode="same")

def trim_leading_zero(*arrays):
    sig = np.zeros(len(arrays[0]), dtype=bool)
    for a in arrays: sig |= (a != 0)
    if not sig.any(): return [a[:0] for a in arrays]
    return [a[np.argmax(sig):] for a in arrays]

fig, axes = plt.subplots(4, 2, figsize=(14, 14), sharex=True)
for ax, p in zip(axes.ravel(), PERIODS):
    path = TAG_FMT.format(p=p, r=R)
    if not os.path.exists(path):
        ax.text(0.5,0.5,f"missing\n{path}",ha="center",va="center",transform=ax.transAxes); continue
    hw, gu, gud, fu, fud = parse(path)
    hw, gu, gud, fu, fud = trim_leading_zero(hw, gu, gud, fu, fud)
    if len(hw) <= WIN:
        ax.text(0.5,0.5,"not enough rows",ha="center",va="center",transform=ax.transAxes); continue
    gu_s, gud_s = smooth(gu), smooth(gud)
    fu_s, fud_s = smooth(fu), smooth(fud)
    diff_g  = gud_s - gu_s
    diff_f  = fud_s - fu_s
    total   = diff_g + diff_f
    ax.plot(hw, diff_g, color="#4C72B0", lw=1.2, label="G(u+δ) − G(u)")
    ax.plot(hw, diff_f, color="#DD8452", lw=1.2, label="F(u+δ) − F(u)")
    ax.plot(hw, total,  color="#55A868", lw=1.5, label="sum")
    ax.axhline(0.0, color="gray", lw=0.7, ls="--")
    row_dt = hw[1]-hw[0] if len(hw)>1 else 0
    ax.set_title(f"segs={p}  (δ={p} rows = {p*row_dt:.3f} TiB)", fontsize=11)
    ax.set_ylabel("Δ", fontsize=10)
    ax.grid(alpha=0.3)
    ax.legend(loc="upper right", fontsize=8)
for ax in axes[-1]:
    ax.set_xlabel("host write t (TiB)", fontsize=10)
fig.suptitle(f"v4 EWMA r=8: ΔG = G(u+δ)−G(u),  ΔF = F(u+δ)−F(u),  sum  (smooth WIN={WIN} rows)", fontsize=13)
fig.tight_layout()
for ext in ("pdf","png"):
    out = f"gs_v4_diff_sum_w{WIN}_r{R}.{ext}"
    fig.savefig(out, bbox_inches="tight")
    print(f"wrote {out}")
