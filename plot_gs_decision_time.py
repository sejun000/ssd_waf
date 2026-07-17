#!/usr/bin/env python3
"""GS hill-climb decision signals over time: LHS = r·(F(u)-F(u+δ))  vs  RHS = G(u+δ)-G(u).

GS rule: LHS > RHS  ⇒  raise target_valid_rate (GC more); else lower (flush more).
2×2 subplot for segs ∈ {1,2,4,8}, r=8.  Two legend lines only.
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
    hw, fu, fud, gu, gud = [], [], [], [], []
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
            except (KeyError, ValueError):
                continue
    return tuple(np.array(a) for a in (hw, fu, fud, gu, gud))

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
    fig, axes = plt.subplots(2, 2, figsize=(14, 9), sharex=True)
    for ax, p in zip(axes.ravel(), PERIODS):
        path = TAG_FMT.format(p=p, r=R)
        if not os.path.exists(path):
            ax.text(0.5, 0.5, f"missing\n{path}", ha="center", va="center",
                    transform=ax.transAxes); continue
        hw, fu, fud, gu, gud = parse(path)
        hw, fu, fud, gu, gud = trim_leading_zero(hw, fu, fud, gu, gud)
        lhs = R * (fu - fud)
        rhs = gud - gu
        lhs_s = smooth(lhs); rhs_s = smooth(rhs)
        ax.plot(hw, lhs_s, label=f"LHS = r·(F(u)−F(u+δ))   r={R}",
                color="#4C72B0", lw=1.5)
        ax.plot(hw, rhs_s, label="RHS = G(u+δ)−G(u)",
                color="#DD8452", lw=1.5)
        ax.axhline(0, color="#888", lw=0.5)
        ax.set_title(f"segs={p}", fontsize=12)
        ax.set_ylabel("rate", fontsize=11)
        ax.grid(alpha=0.3)
        ax.legend(loc="upper left", fontsize=10)
    for ax in axes[-1]:
        ax.set_xlabel("host write (TiB)", fontsize=11)
    fig.suptitle(f"GS hill-climb: LHS vs RHS over time   r={R}, hl=1.57M, dwpd1to2_4x ×2",
                 fontsize=13)
    fig.tight_layout()
    for ext in ("pdf", "png"):
        out = f"gs_decision_time_r{R}.{ext}"
        fig.savefig(out, bbox_inches="tight")
        print(f"wrote {out}")

if __name__ == "__main__":
    main()
