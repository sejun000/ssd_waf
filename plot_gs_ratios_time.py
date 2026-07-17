#!/usr/bin/env python3
"""Time-series of F(u), F(u+delta), G(u), G(u+delta) for segs in {1,2,4,8}, r=8.

X: host write (TiB).  Stat rows are written every 6 GiB.
"""
import os, re, sys
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

PERIODS = [1, 2, 4, 8]
R       = 8
TAG_FMT = "LOG_GREEDY_COST_BENEFIT_10_GS_us02_ewma_hl1572864_segs{p}.stat_pr{r}"
KEY_RE  = re.compile(r"(\w+):?\s+(-?\d+(?:\.\d+)?)")
TIB     = 1024**4

def parse(path):
    hw, fu, fud, gu, gud, tgt, us = [], [], [], [], [], [], []
    with open(path) as fh:
        for line in fh:
            if not line.startswith("LOG_GREEDY"):
                continue
            kv = dict(KEY_RE.findall(line))
            try:
                hw .append(int(kv["write_size_to_cache"]) / TIB)
                fu .append(float(kv["F_u"]))
                fud.append(float(kv["F_u_delta"]))
                gu .append(float(kv["G_u"]))
                gud.append(float(kv["G_u_delta"]))
                tgt.append(float(kv["target_valid_rate"]))
                us .append(float(kv["util_step"]))
            except (KeyError, ValueError):
                continue
    return np.array(hw), np.array(fu), np.array(fud), np.array(gu), np.array(gud), np.array(tgt), np.array(us)

WIN = 30  # rolling-mean window in rows (rows ~ 6 GiB each)

def smooth(y, win=WIN):
    if len(y) < win:
        return y
    k = np.ones(win) / win
    return np.convolve(y, k, mode="same")

def trim_leading_zero(*arrays):
    """Skip rows where *all* signals are exactly 0 (pre-warmup)."""
    sig = np.zeros(len(arrays[0]), dtype=bool)
    for a in arrays:
        sig |= (a != 0)
    if not sig.any():
        return [a[:0] for a in arrays]
    first = np.argmax(sig)
    return [a[first:] for a in arrays]

def plot_masked(ax, x, y, **kw):
    """Plot y vs x, masking points where y == 0 (NaN gap)."""
    ym = np.where(y > 0, y, np.nan)
    ax.plot(x, ym, **kw)

def main():
    fig, axes = plt.subplots(2, 2, figsize=(14, 9), sharex=True)
    for ax, p in zip(axes.ravel(), PERIODS):
        path = TAG_FMT.format(p=p, r=R)
        if not os.path.exists(path):
            ax.text(0.5, 0.5, f"missing\n{path}", ha="center", va="center",
                    transform=ax.transAxes); continue
        hw, fu, fud, gu, gud, tgt, us = parse(path)
        # trim warmup where every ratio is still 0
        hw, fu, fud, gu, gud, tgt, us = trim_leading_zero(hw, fu, fud, gu, gud, tgt, us)
        fu_s, fud_s, gu_s, gud_s = smooth(fu), smooth(fud), smooth(gu), smooth(gud)
        plot_masked(ax, hw, fu_s,  label="F(u)",   color="#4C72B0", lw=1.4)
        plot_masked(ax, hw, fud_s, label="F(u+δ)", color="#4C72B0", lw=1.4, ls="--")
        plot_masked(ax, hw, gu_s,  label="G(u)",   color="#DD8452", lw=1.4)
        plot_masked(ax, hw, gud_s, label="G(u+δ)", color="#DD8452", lw=1.4, ls="--")
        # target overlay on twin axis
        ax2 = ax.twinx()
        ax2.plot(hw, tgt, label="target_valid", color="#55A868", lw=1.0, alpha=0.7)
        ax2.set_ylim(0, 1.0)
        ax2.set_ylabel("target_valid_rate", color="#55A868", fontsize=11)
        ax2.tick_params(axis="y", colors="#55A868")
        us_val = us[-1] if len(us) else 0
        ax.set_title(f"segs={p}  (util_step={us_val:.4f})", fontsize=12)
        ax.set_ylabel("rate", fontsize=11)
        ax.grid(alpha=0.3)
        ax.legend(loc="upper left", fontsize=9)
    for ax in axes[-1]:
        ax.set_xlabel("host write (TiB)", fontsize=11)
    fig.suptitle(f"GS hill-climb signals over time   r={R}, hl=1.57M, dwpd1to2_4x scale=2",
                 fontsize=13)
    fig.tight_layout()
    for ext in ("pdf", "png"):
        out = f"gs_ratios_time_r{R}.{ext}"
        fig.savefig(out, bbox_inches="tight")
        print(f"wrote {out}")

if __name__ == "__main__":
    main()
