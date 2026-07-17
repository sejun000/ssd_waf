#!/usr/bin/env python3
"""Stacked-bar of f = r·flush + comp across 6 EWMA half-life multipliers, GC policy."""
import os, re, sys
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

WINDOWS = [
    (0.25, 1572864,  "LOG_GREEDY_COST_BENEFIT_10_GC_us02_ewma_hl1572864"),
    (0.5,  3145728,  "LOG_GREEDY_COST_BENEFIT_10_GC_us02_ewma_hl3145728"),
    (1.0,  6291456,  "LOG_GREEDY_COST_BENEFIT_10_GC_us02_ewma_fix"),
    (2.0,  12582912, "LOG_GREEDY_COST_BENEFIT_10_GC_us02_ewma_hl12582912"),
    (4.0,  25165824, "LOG_GREEDY_COST_BENEFIT_10_GC_us02_ewma_hl25165824"),
    (8.0,  50331648, "LOG_GREEDY_COST_BENEFIT_10_GC_us02_ewma_hl50331648"),
]
RS = [2, 4, 6, 8, 10]
WARMUP = 6 * 1024**4
BS = 4096
KEY_RE = re.compile(r"(\w+):?\s+(-?\d+(?:\.\d+)?)")

def parse(p):
    last = warm_first = None
    with open(p) as fh:
        for line in fh:
            if not line.startswith("LOG_GREEDY"): continue
            kv = dict(KEY_RE.findall(line))
            try:
                w=int(kv["write_size_to_cache"]); e=int(kv["evicted_blocks"]); c=int(kv["compacted_blocks"])
            except: continue
            if w>=WARMUP and warm_first is None: warm_first=(w,e,c)
            last=(w,e,c)
    if not warm_first or not last: return None
    h=(last[0]-warm_first[0])/BS
    if h<=0: return None
    return (last[1]-warm_first[1])/h, (last[2]-warm_first[2])/h

def main():
    data = {}
    for mult, hl, tag in WINDOWS:
        for r in RS:
            p = f"{tag}.stat_pr{r}"
            if not os.path.exists(p):
                print(f"missing: {p}", file=sys.stderr); continue
            v = parse(p)
            if v is None:
                continue
            data[(mult, r)] = v

    fig, ax = plt.subplots(figsize=(13, 5.5))
    n_win = len(WINDOWS)
    bw = 0.13
    xs = np.arange(len(RS))

    cmap = plt.get_cmap("viridis")
    flush_colors = [cmap(i / (n_win - 1) * 0.55 + 0.05) for i in range(n_win)]
    comp_colors  = [cmap(i / (n_win - 1) * 0.55 + 0.55) for i in range(n_win)]

    for i, (mult, hl, tag) in enumerate(WINDOWS):
        flush_part = []
        comp_part  = []
        for r in RS:
            v = data.get((mult, r))
            if v is None:
                flush_part.append(0); comp_part.append(0)
            else:
                fl, cp = v
                flush_part.append(r * fl)
                comp_part.append(cp)
        flush_part = np.array(flush_part)
        comp_part  = np.array(comp_part)
        offset = (i - (n_win - 1) / 2) * bw
        suffix = "*" if mult == 1.0 else ""
        ax.bar(xs + offset, flush_part, bw,
               color=flush_colors[i], edgecolor="black", linewidth=0.4,
               label=f"{mult}x{suffix} r·flush")
        ax.bar(xs + offset, comp_part, bw, bottom=flush_part,
               color=comp_colors[i], edgecolor="black", linewidth=0.4,
               label=f"{mult}x{suffix} comp")
        for x, fl, cp in zip(xs + offset, flush_part, comp_part):
            tot = fl + cp
            if tot > 0:
                ax.text(x, tot + 0.04, f"{tot:.2f}",
                        ha="center", va="bottom", fontsize=6, rotation=90)

    ax.set_xticks(xs)
    ax.set_xticklabels(RS)
    ax.set_xlabel("periodic_ratio  r")
    ax.set_ylabel("f = r·flush + comp")
    ax.set_title("GC policy: EWMA half-life sweep — stacked f = r·(evict/host) + (comp/host)\n"
                 "(us=0.02; * = pre-existing baseline)")
    ax.grid(True, axis="y", alpha=0.3)
    ax.legend(fontsize=7, ncol=6, loc="upper left", framealpha=0.9)
    fig.tight_layout()
    for ext in ("pdf", "png"):
        out = f"halflife_stacked.{ext}"
        fig.savefig(out, bbox_inches="tight")
        print(f"wrote {out}")

if __name__ == "__main__":
    main()
