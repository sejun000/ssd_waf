#!/usr/bin/env python3
"""Fair pred-vs-real relative err over time for v4 EWMA r=8 sweep.
WIN per panel = δ × kFactor so WIN/δ ratio is identical (= kFactor) across all
panels — eliminates the trivial smoothing advantage that small-δ enjoys with
a fixed-WIN setup.
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
FACTOR  = 4   # WIN = δ × FACTOR rows

def parse(path):
    hw, gu, gud = [], [], []
    with open(path) as fh:
        for line in fh:
            if not line.startswith("LOG_GREEDY"): continue
            kv = dict(KEY_RE.findall(line))
            try:
                hw .append(int(kv["write_size_to_cache"]) / TIB)
                gu .append(float(kv["G_u"]))
                gud.append(float(kv["G_u_delta"]))
            except (KeyError, ValueError): continue
    return tuple(np.array(a) for a in (hw, gu, gud))

def smooth(y, win):
    if win < 2 or len(y) < win: return y
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
    hw, gu, gud = parse(path)
    hw, gu, gud = trim_leading_zero(hw, gu, gud)
    win = max(p * FACTOR, 2)
    if len(hw) <= max(p, win):
        ax.text(0.5,0.5,"not enough rows",ha="center",va="center",transform=ax.transAxes); continue
    gu_s  = smooth(gu, win)
    gud_s = smooth(gud, win)
    t     = hw[:-p]
    pred  = gud_s[:-p]
    real  = gu_s [p:]
    with np.errstate(divide="ignore", invalid="ignore"):
        err = np.where(np.abs(real) > 1e-6, (pred - real) / real, np.nan)
    ax.plot(t, err, color="#4C72B0", lw=1.2,
            label=f"(pred−real)/real  WIN={win} rows  WIN/δ=4")
    ax.axhline(0.0, color="#DD8452", lw=0.9, ls="--", label="err=0")
    ax.set_ylim(-2, 2)
    row_dt = hw[1]-hw[0] if len(hw)>1 else 0
    ax.set_title(f"segs={p}  (δ={p} rows = {p*row_dt:.3f} TiB,  smooth WIN={win} rows)", fontsize=11)
    ax.set_ylabel("(pred-real)/real", fontsize=10)
    ax.grid(alpha=0.3)
    ax.legend(loc="upper right", fontsize=8)
for ax in axes[-1]:
    ax.set_xlabel("host write t (TiB)", fontsize=10)
fig.suptitle(f"v4 EWMA r=8: smoothed-first relerr, WIN/δ = {FACTOR} (fair across panels)", fontsize=13)
fig.tight_layout()
for ext in ("pdf","png"):
    out = f"gs_v4_relerr_fair_r{R}.{ext}"
    fig.savefig(out, bbox_inches="tight")
    print(f"wrote {out}")
