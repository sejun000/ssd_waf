#!/usr/bin/env python3
"""pred vs real for v4 EWMA r=2.88 sweep, segs ∈ {1,2,4,8,16}.
Smoothing window = 4 rows (= 4 segments host write).
"""
import os, re
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

PERIODS = [1, 2, 4, 8, 16]
R_TAG   = 288
TAG_FMT = "LOG_GREEDY_COST_BENEFIT_10_GS_us02_ewma_hl1572864_gsv4_segs{p}.stat_pr{r}"
KEY_RE  = re.compile(r"(\w+):?\s+(-?\d+(?:\.\d+)?)")
TIB     = 1024**4
WIN     = 32

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

def smooth(y, win=WIN):
    if len(y) < win: return y
    return np.convolve(y, np.ones(win)/win, mode="same")

def trim_leading_zero(*arrays):
    sig = np.zeros(len(arrays[0]), dtype=bool)
    for a in arrays: sig |= (a != 0)
    if not sig.any(): return [a[:0] for a in arrays]
    return [a[np.argmax(sig):] for a in arrays]

def main():
    fig, axes = plt.subplots(3, 2, figsize=(14, 11), sharex=True)
    axs = axes.ravel()
    for ax, p in zip(axs, PERIODS):
        path = TAG_FMT.format(p=p, r=R_TAG)
        if not os.path.exists(path):
            ax.text(0.5,0.5,f"missing\n{path}",ha="center",va="center",transform=ax.transAxes); continue
        hw, gu, gud = parse(path)
        hw, gu, gud = trim_leading_zero(hw, gu, gud)
        if len(hw) <= p:
            ax.text(0.5,0.5,"not enough rows",ha="center",va="center",transform=ax.transAxes); continue
        gu_s  = smooth(gu)
        gud_s = smooth(gud)
        t = hw[:-p]
        ax.plot(t, gud_s[:-p], color="#4C72B0", lw=1.3, label="G_t(u+δ)  pred")
        ax.plot(t, gu_s [p:],  color="#DD8452", lw=1.3, label="G_(t+δ)(u) realized")
        row_dt = hw[1]-hw[0] if len(hw)>1 else 0
        ax.set_title(f"segs={p}  (δ={p} rows={p*row_dt:.3f} TiB)", fontsize=11)
        ax.set_ylabel("G", fontsize=10)
        ax.grid(alpha=0.3)
        ax.legend(loc="upper right", fontsize=8)
    # hide unused last panel
    axs[-1].axis("off")
    for ax in axes[-1]:
        ax.set_xlabel("host write t (TiB)", fontsize=10)
    fig.suptitle(f"v4 EWMA r=2.88: pred vs realized (smooth win={WIN} rows = {WIN} segments)", fontsize=13)
    fig.tight_layout()
    for ext in ("pdf","png"):
        out = f"gs_v4_pred_vs_real_w{WIN}_r{R_TAG}.{ext}"
        fig.savefig(out, bbox_inches="tight")
        print(f"wrote {out}")

if __name__ == "__main__":
    main()
