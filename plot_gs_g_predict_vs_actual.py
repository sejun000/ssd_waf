#!/usr/bin/env python3
"""Predicted G(u+δ) vs realized G(u): shift G(u+δ) forward by δ rows so the
prediction made at time t lines up with the realized G(u) at t+δ.

stat row interval = 6 GiB = 1 segment.  util_step = segs/N_segs, so a δ-shift
in host-write space corresponds to exactly `segs` stat rows.

If GS's ghost estimator is accurate, the shifted G(u+δ) should track G(u).
"""
import os, re
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

PERIODS = [1, 2, 4, 8]
R       = 8
TAG_FMT = "LOG_GREEDY_COST_BENEFIT_10_GS_us02_ewma_hl1572864_segs{p}.stat_pr{r}"
KEY_RE  = re.compile(r"(\w+):?\s+(-?\d+(?:\.\d+)?)")
TIB     = 1024**4
WIN     = 30

def parse(path):
    hw, gu, gud = [], [], []
    with open(path) as fh:
        for line in fh:
            if not line.startswith("LOG_GREEDY"):
                continue
            kv = dict(KEY_RE.findall(line))
            try:
                hw .append(int(kv["write_size_to_cache"]) / TIB)
                gu .append(float(kv["G_u"]))
                gud.append(float(kv["G_u_delta"]))
            except (KeyError, ValueError):
                continue
    return tuple(np.array(a) for a in (hw, gu, gud))

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
    ym = np.where(y > 0, y, np.nan)
    ax.plot(x, ym, **kw)

def main():
    fig, axes = plt.subplots(2, 2, figsize=(14, 9), sharex=True)
    for ax, p in zip(axes.ravel(), PERIODS):
        path = TAG_FMT.format(p=p, r=R)
        if not os.path.exists(path):
            ax.text(0.5, 0.5, f"missing\n{path}", ha="center", va="center",
                    transform=ax.transAxes); continue
        hw, gu, gud = parse(path)
        hw, gu, gud = trim_leading_zero(hw, gu, gud)
        # Shift G(u+δ) forward by `p` rows in the X axis so the prediction at
        # time t is plotted at t+δ — alongside the realized G(u) at t+δ.
        row_dt   = hw[1] - hw[0] if len(hw) > 1 else 0
        shift_x  = hw + p * row_dt
        gu_s     = smooth(gu)
        gud_s    = smooth(gud)
        plot_masked(ax, hw,      gu_s,
                    label="G(u) at time t",      color="#DD8452", lw=1.5)
        plot_masked(ax, shift_x, gud_s,
                    label=f"G(u+δ) predicted at t−δ\n(shifted +{p} rows = +{p*row_dt:.3f} TiB)",
                    color="#4C72B0", lw=1.5, ls="--")
        ax.set_title(f"segs={p}", fontsize=12)
        ax.set_ylabel("GC rate", fontsize=11)
        ax.grid(alpha=0.3)
        ax.legend(loc="upper left", fontsize=9)
    for ax in axes[-1]:
        ax.set_xlabel("host write (TiB)", fontsize=11)
    fig.suptitle(f"GS ghost-G prediction vs realized   r={R}, hl=1.57M, dwpd1to2_4x ×2",
                 fontsize=13)
    fig.tight_layout()
    for ext in ("pdf", "png"):
        out = f"gs_g_predict_vs_actual_r{R}.{ext}"
        fig.savefig(out, bbox_inches="tight")
        print(f"wrote {out}")

if __name__ == "__main__":
    main()
