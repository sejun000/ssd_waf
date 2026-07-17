#!/usr/bin/env python3
"""At time t: compare G_t(u) (current), G_t(u+δ) (prediction), and G_{t+δ}(u)
(realized G after δ).  All three on the same X=t so the dashed/realized pair
shows ghost-estimator accuracy directly.

δ in stat rows = `segs` (we chose util_step = segs/N_segs and stat rows are
1-segment spaced, so the row count equals segs).
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
    ym = np.where(np.isfinite(y) & (y > 0), y, np.nan)
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

        # Smooth before slicing so the rolling-mean tail isn't dominated by zeros.
        gu_s  = smooth(gu)
        gud_s = smooth(gud)

        # Align everything to t = hw[:-p].  At index i:
        #   gu_now[i]    = G_t(u)     at time t = hw[i]
        #   gud_now[i]   = G_t(u+δ)   prediction made at t
        #   gu_future[i] = G_{t+δ}(u) realized δ rows later
        if p == 0 or len(hw) <= p:
            continue
        t_axis    = hw[:-p]
        gu_now    = gu_s [:-p]
        gud_now   = gud_s[:-p]
        gu_future = gu_s [p:]

        plot_masked(ax, t_axis, gu_now,
                    label="G_t(u)        (current)",     color="#888888", lw=1.0)
        plot_masked(ax, t_axis, gud_now,
                    label="G_t(u+δ)     (prediction)",   color="#4C72B0", lw=1.7, ls="--")
        plot_masked(ax, t_axis, gu_future,
                    label="G_{t+δ}(u)   (realized)",      color="#DD8452", lw=1.5)

        row_dt = hw[1] - hw[0] if len(hw) > 1 else 0
        ax.set_title(f"segs={p}  (δ = {p} rows = {p*row_dt:.3f} TiB)", fontsize=12)
        ax.set_ylabel("GC rate", fontsize=11)
        ax.grid(alpha=0.3)
        ax.legend(loc="upper left", fontsize=9)
    for ax in axes[-1]:
        ax.set_xlabel("host write t (TiB)", fontsize=11)
    fig.suptitle(f"Ghost prediction accuracy: G_t(u+δ) vs G_{{t+δ}}(u)   r={R}, hl=1.57M, dwpd1to2_4x ×2",
                 fontsize=13)
    fig.tight_layout()
    for ext in ("pdf", "png"):
        out = f"gs_g_prediction_vs_realized_r{R}.{ext}"
        fig.savefig(out, bbox_inches="tight")
        print(f"wrote {out}")

if __name__ == "__main__":
    main()
