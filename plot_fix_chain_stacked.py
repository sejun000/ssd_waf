#!/usr/bin/env python3
"""Stacked bar of f = r*flush + comp for the post-fix sweep.
3 policies × 2 MA × r ∈ {2,4,6,8,10}."""
import os, re, sys
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

POLICIES = [
    ("LOG_GREEDY_COST_BENEFIT_10",        "GD"),
    ("LOG_GREEDY_COST_BENEFIT_10_TDELTA", "TDELTA"),
    ("LOG_GREEDY_COST_BENEFIT_10_GC",     "GC"),
]
MAS = ["ewma", "sma"]
RS = [2, 4, 6, 8, 10]
WARMUP_BYTES = 6 * 1024**4
BLOCK_BYTES = 4096
KEY_RE = re.compile(r"(\w+):?\s+(-?\d+(?:\.\d+)?)")

def parse(path):
    last = warm_first = None
    with open(path) as fh:
        for line in fh:
            if not line.startswith("LOG_GREEDY"):
                continue
            kv = dict(KEY_RE.findall(line))
            try:
                wsc = int(kv["write_size_to_cache"])
                ev  = int(kv["evicted_blocks"])
                cp  = int(kv["compacted_blocks"])
            except KeyError:
                continue
            if wsc >= WARMUP_BYTES and warm_first is None:
                warm_first = (wsc, ev, cp)
            last = (wsc, ev, cp)
    if not warm_first or not last:
        return None
    h = (last[0] - warm_first[0]) / BLOCK_BYTES
    e = last[1] - warm_first[1]
    c = last[2] - warm_first[2]
    if h <= 0:
        return None
    return e/h, c/h  # flush_rate, comp_rate

def gather():
    out = {}
    for pol, _ in POLICIES:
        for ma in MAS:
            for r in RS:
                p = f"{pol}_us02_{ma}_fix.stat_pr{r}"
                if not os.path.exists(p):
                    print(f"missing: {p}", file=sys.stderr); continue
                v = parse(p)
                if v is None:
                    continue
                out[(pol, ma, r)] = v
    return out

def main():
    data = gather()
    fig, axes = plt.subplots(1, 3, figsize=(16, 5.2), sharey=True)
    xs = np.arange(len(RS))
    bw = 0.36

    color_flush = "#4C72B0"   # blue
    color_comp  = "#DD8452"   # orange

    for ax, (pol, label) in zip(axes, POLICIES):
        for i, ma in enumerate(MAS):
            flush_part = []
            comp_part  = []
            for r in RS:
                v = data.get((pol, ma, r))
                if v is None:
                    flush_part.append(0); comp_part.append(0)
                else:
                    fl, cp = v
                    flush_part.append(r * fl)
                    comp_part.append(cp)
            flush_part = np.array(flush_part)
            comp_part  = np.array(comp_part)
            offset = (i - 0.5) * bw
            hatch = None if ma == "ewma" else "//"
            edge = "black"
            b1 = ax.bar(xs + offset, flush_part, bw,
                        color=color_flush, edgecolor=edge, linewidth=0.5,
                        hatch=hatch,
                        label=f"r·flush ({ma.upper()})" if ax is axes[0] else None)
            b2 = ax.bar(xs + offset, comp_part, bw, bottom=flush_part,
                        color=color_comp, edgecolor=edge, linewidth=0.5,
                        hatch=hatch,
                        label=f"comp ({ma.upper()})" if ax is axes[0] else None)
            for x, fl, cp in zip(xs + offset, flush_part, comp_part):
                total = fl + cp
                if total > 0:
                    ax.text(x, total + 0.05, f"{total:.2f}",
                            ha="center", va="bottom", fontsize=7)
        ax.set_title(label)
        ax.set_xticks(xs)
        ax.set_xticklabels(RS)
        ax.set_xlabel("periodic_ratio  r")
        ax.grid(True, axis="y", alpha=0.3)

    axes[0].set_ylabel("f = r·flush + comp")
    axes[0].legend(fontsize=8, loc="upper left", framealpha=0.9)
    fig.suptitle("Post-fix sweep stacked: f = r·(evict/host) + (comp/host)   "
                 "[us=0.02, EWMA solid / SMA hatched]",
                 y=1.02, fontsize=11)
    fig.tight_layout()
    for ext in ("pdf", "png"):
        out = f"fix_chain_stacked.{ext}"
        fig.savefig(out, bbox_inches="tight")
        print(f"wrote {out}")

if __name__ == "__main__":
    main()
