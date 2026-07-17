#!/usr/bin/env python3
"""Plot LRU MRC curves for the dwpd hi/mid/lo traces.

Reads the CSV files emitted by mrc_calculator:
    IntervalBytes,CacheSize(blocks),MissRate(%)
If a file has multiple intervals, only the largest prefix (full trace) is used.

Usage:
    python3 mrc_plot.py hi=mrc_hi.csv mid=mrc_mid.csv lo=mrc_lo.csv [--out mrc_curves]
"""
import sys
import csv
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

BLOCK = 4096  # bytes per block (matches mrc_main.cpp)

COLORS = {"hi": "#d62728", "mid": "#2ca02c", "lo": "#1f77b4"}


def load(path):
    """Return (xs_TB, ys_pct) for the largest IntervalBytes group in the file."""
    rows = []
    with open(path, newline="") as f:
        r = csv.reader(f)
        header = next(r, None)
        for line in r:
            if len(line) < 3:
                continue
            try:
                ib = int(line[0]); cb = int(line[1]); mr = float(line[2])
            except ValueError:
                continue
            rows.append((ib, cb, mr))
    if not rows:
        return [], [], 0
    last_iv = max(r[0] for r in rows)          # full-trace prefix
    pts = sorted((cb, mr) for ib, cb, mr in rows if ib == last_iv)
    xs = [cb * BLOCK / 1e12 for cb, _ in pts]  # cache size in TB
    ys = [mr for _, mr in pts]
    return xs, ys, last_iv


def main():
    series = {}
    out = "mrc_curves"
    for a in sys.argv[1:]:
        if a.startswith("--out"):
            out = a.split("=", 1)[1] if "=" in a else None
            continue
        if "=" in a:
            label, path = a.split("=", 1)
            series[label] = path
    if not series:
        print("no inputs; e.g. hi=mrc_hi.csv mid=mrc_mid.csv lo=mrc_lo.csv")
        return 1

    loaded = {}
    for label, path in series.items():
        try:
            xs, ys, iv = load(path)
        except FileNotFoundError:
            print(f"skip {label}: {path} not found")
            continue
        if not xs:
            print(f"skip {label}: no data in {path}")
            continue
        loaded[label] = (xs, ys, iv)
        print(f"{label}: {len(xs)} pts, full-trace={iv/1e12:.2f}TB, "
              f"miss@max={ys[-1]:.2f}% miss@min={ys[0]:.2f}%")

    if not loaded:
        print("nothing to plot")
        return 1

    fig, axes = plt.subplots(1, 2, figsize=(14, 5.5))
    for ax, logx in zip(axes, (False, True)):
        for label, (xs, ys, _) in loaded.items():
            ax.plot(xs, ys, label=label, lw=2,
                    color=COLORS.get(label))
        ax.set_xlabel("Cache size (TB)")
        ax.set_ylabel("Miss rate (%)")
        ax.set_ylim(0, 100)
        ax.grid(True, alpha=0.3)
        ax.legend(title="trace")
        if logx:
            ax.set_xscale("log")
            ax.set_title("LRU MRC (log-x)")
        else:
            ax.set_title("LRU MRC (linear-x)")
    fig.suptitle("LRU Miss-Ratio Curves: dwpd hi / mid / lo", fontsize=13)
    fig.tight_layout()
    for ext in ("png", "pdf"):
        fig.savefig(f"{out}.{ext}", dpi=130, bbox_inches="tight")
        print(f"wrote {out}.{ext}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
