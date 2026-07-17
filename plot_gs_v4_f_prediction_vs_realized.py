#!/usr/bin/env python3
"""v4: F_t(u+δ) predicted vs F_{t+δ}(u) realized.  Same layout as the G plot
but for the flush-rate channel.  r=8, segs ∈ {1,...,128}.
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
WIN     = 30

def parse(path):
    hw, fu, fud = [], [], []
    with open(path) as fh:
        for line in fh:
            if not line.startswith("LOG_GREEDY"):
                continue
            kv = dict(KEY_RE.findall(line))
            try:
                hw .append(int(kv["write_size_to_cache"]) / TIB)
                fu .append(float(kv["F_u"]))
                fud.append(float(kv["F_u_delta"]))
            except (KeyError, ValueError):
                continue
    return tuple(np.array(a) for a in (hw, fu, fud))

def smooth(y, win=WIN):
    if len(y) < win:
        return y
    return np.convolve(y, np.ones(win) / win, mode="same")

def trim_leading_zero(*arrays):
    sig = np.zeros(len(arrays[0]), dtype=bool)
    for a in arrays:
        sig |= (a != 0)
    if not sig.any():
        return [a[:0] for a in arrays]
    return [a[np.argmax(sig):] for a in arrays]

def plot_masked(ax, x, y, **kw):
    ym = np.where(np.isfinite(y) & (y > 0), y, np.nan)
    ax.plot(x, ym, **kw)

def main():
    fig, axes = plt.subplots(4, 2, figsize=(14, 14), sharex=True)
    for ax, p in zip(axes.ravel(), PERIODS):
        path = TAG_FMT.format(p=p, r=R)
        if not os.path.exists(path):
            ax.text(0.5, 0.5, f"missing\n{path}", ha="center", va="center",
                    transform=ax.transAxes); continue
        hw, fu, fud = parse(path)
        hw, fu, fud = trim_leading_zero(hw, fu, fud)
        if len(hw) <= p:
            ax.text(0.5, 0.5, f"not enough rows\n(p={p}, len={len(hw)})",
                    ha="center", va="center", transform=ax.transAxes); continue
        fu_s  = smooth(fu)
        fud_s = smooth(fud)
        t_axis    = hw[:-p]
        fu_now    = fu_s [:-p]
        fud_now   = fud_s[:-p]
        fu_future = fu_s [p:]
        plot_masked(ax, t_axis, fu_now,
                    label="F_t(u)        (current)",   color="#888888", lw=1.0)
        plot_masked(ax, t_axis, fud_now,
                    label="F_t(u+δ)     (prediction)", color="#4C72B0", lw=1.7, ls="--")
        plot_masked(ax, t_axis, fu_future,
                    label="F_{t+δ}(u)   (realized)",   color="#DD8452", lw=1.5)
        row_dt = hw[1] - hw[0] if len(hw) > 1 else 0
        ax.set_title(f"segs={p}  (δ = {p} rows = {p*row_dt:.3f} TiB)", fontsize=11)
        ax.set_ylabel("flush rate", fontsize=10)
        ax.grid(alpha=0.3)
        ax.legend(loc="upper left", fontsize=8)
    for ax in axes[-1]:
        ax.set_xlabel("host write t (TiB)", fontsize=10)
    fig.suptitle(f"v4: ghost prediction accuracy  F_t(u+δ) vs F_{{t+δ}}(u)   r={R}",
                 fontsize=13)
    fig.tight_layout()
    for ext in ("pdf", "png"):
        out = f"gs_v4_f_prediction_vs_realized_r{R}.{ext}"
        fig.savefig(out, bbox_inches="tight")
        print(f"wrote {out}")

if __name__ == "__main__":
    main()
