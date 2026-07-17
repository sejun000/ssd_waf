#!/usr/bin/env python3
"""8-panel pred vs real for v4 r=8 sweep, all segs ∈ {1,2,4,8,16,32,64,128}.
Common moving-average window WIN = 128 rows (= segs=128 δ = 768 GiB).
pred  = G_t(u+δ) at time t        : gud[i]
real  = G_(t+δ)(u) at time t+δ    : gu[i + segs]
Both smoothed with the same WIN and plotted on a common x = t.
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
WIN     = 128   # rows; 1 row = 6 GiB → WIN = 768 GiB

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

def main():
    fig, axes = plt.subplots(4, 2, figsize=(14, 14), sharex=True)
    for ax, p in zip(axes.ravel(), PERIODS):
        path = TAG_FMT.format(p=p, r=R)
        if not os.path.exists(path):
            ax.text(0.5, 0.5, f"missing\n{path}", ha="center", va="center",
                    transform=ax.transAxes); continue
        hw, gu, gud = parse(path)
        hw, gu, gud = trim_leading_zero(hw, gu, gud)
        if len(hw) <= p:
            ax.text(0.5, 0.5, "not enough rows", ha="center", va="center",
                    transform=ax.transAxes); continue
        gu_s  = smooth(gu)
        gud_s = smooth(gud)
        t_axis = hw[:-p]
        pred   = gud_s[:-p]
        real   = gu_s [p:]
        ax.plot(t_axis, pred, color="#4C72B0", lw=1.4,
                label="G_t(u+δ)  pred")
        ax.plot(t_axis, real, color="#DD8452", lw=1.4,
                label="G_(t+δ)(u)  realized")
        row_dt = hw[1] - hw[0] if len(hw) > 1 else 0
        ax.set_title(f"segs={p}  (δ = {p} rows = {p*row_dt:.3f} TiB)", fontsize=11)
        ax.set_ylabel("G", fontsize=10)
        ax.grid(alpha=0.3)
        ax.legend(loc="upper right", fontsize=8)
    for ax in axes[-1]:
        ax.set_xlabel("host write t (TiB)", fontsize=10)
    fig.suptitle(f"v4: pred vs real (smooth win = {WIN} rows = {WIN*6/1024:.2f} TiB)   r={R}, hl=1.57M",
                 fontsize=13)
    fig.tight_layout()
    for ext in ("pdf", "png"):
        out = f"gs_v4_pred_vs_real_w{WIN}_all_r{R}.{ext}"
        fig.savefig(out, bbox_inches="tight")
        print(f"wrote {out}")

if __name__ == "__main__":
    main()
