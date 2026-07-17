#!/usr/bin/env python3
"""Single-panel: smoothed G_t(u+δ) prediction vs smoothed G_{t+δ}(u)
realized, segs=128 (δ = 128 rows = 768 GiB) for v4 r=8.
WIN = 100 rows (= 600 GiB) moving average. row = 6 GiB.
"""
import os, re
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

SEGS    = 128
R       = 8
PATH    = f"LOG_GREEDY_COST_BENEFIT_10_GS_us02_ewma_hl1572864_gsv4_segs{SEGS}.stat_pr{R}"
KEY_RE  = re.compile(r"(\w+):?\s+(-?\d+(?:\.\d+)?)")
TIB     = 1024**4
WIN     = 100

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
    if not os.path.exists(PATH):
        raise SystemExit(f"missing stat file: {PATH}")
    hw, gu, gud = parse(PATH)
    hw, gu, gud = trim_leading_zero(hw, gu, gud)
    p = SEGS
    if len(hw) <= p:
        raise SystemExit("not enough rows")

    gu_s  = smooth(gu)
    gud_s = smooth(gud)
    # pred made at t = gud[i], realized at t+δ = gu[i+p]
    t_axis = hw[:-p]
    pred   = gud_s[:-p]
    real   = gu_s [p:]

    fig, ax = plt.subplots(figsize=(11, 5.2))
    ax.plot(t_axis, pred, color="#4C72B0", lw=1.6,
            label=f"G_t(u+δ)  pred  (smooth win={WIN})")
    ax.plot(t_axis, real, color="#DD8452", lw=1.6,
            label=f"G_(t+δ)(u)  realized (smooth win={WIN})")
    row_dt = hw[1] - hw[0] if len(hw) > 1 else 0
    ax.set_title(f"v4  segs={SEGS}  δ = {SEGS} rows = {SEGS*row_dt:.3f} TiB   r={R}, hl=1.57M")
    ax.set_xlabel("host write t (TiB)")
    ax.set_ylabel("G (GC rate)")
    ax.grid(alpha=0.3)
    ax.legend(loc="upper right", fontsize=10)
    fig.tight_layout()
    for ext in ("pdf", "png"):
        out = f"gs_v4_pred_vs_real_s{SEGS}_r{R}.{ext}"
        fig.savefig(out, bbox_inches="tight")
        print(f"wrote {out}")

if __name__ == "__main__":
    main()
