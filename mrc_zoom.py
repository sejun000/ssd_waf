#!/usr/bin/env python3
"""Zoomed linear MRC plot for dwpd hi/mid/lo.

Reuses load() from mrc_plot.py. Defaults: x in [0, 2.5 TB], x-ticks every 200 GB.

Usage:
    python3 mrc_zoom.py [hi=mrc_hi.csv mid=mrc_mid.csv lo=mrc_lo.csv]
                        [--xmax-tb=2.5] [--tick-gb=200] [--out=mrc_curves_zoom]
"""
import sys
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from mrc_plot import load, COLORS


def main():
    series = {}
    xmax_tb = 2.5
    tick_gb = 200
    out = "mrc_curves_zoom"
    for a in sys.argv[1:]:
        if a.startswith("--xmax-tb="):
            xmax_tb = float(a.split("=", 1)[1])
        elif a.startswith("--tick-gb="):
            tick_gb = float(a.split("=", 1)[1])
        elif a.startswith("--out="):
            out = a.split("=", 1)[1]
        elif "=" in a:
            label, path = a.split("=", 1)
            series[label] = path
    if not series:
        series = {"hi": "mrc_hi.csv", "mid": "mrc_mid.csv", "lo": "mrc_lo.csv"}

    xmax_gb = xmax_tb * 1000.0
    fig, ax = plt.subplots(figsize=(10, 6.5))
    for label, path in series.items():
        try:
            xs_tb, ys, iv = load(path)
        except FileNotFoundError:
            print(f"skip {label}: {path} not found")
            continue
        if not xs_tb:
            print(f"skip {label}: no data")
            continue
        xs_gb = [x * 1000.0 for x in xs_tb]  # TB -> GB
        # keep only the zoom window (+1 point past the edge for a clean line)
        px, py = [], []
        for x, y in zip(xs_gb, ys):
            px.append(x); py.append(y)
            if x > xmax_gb:
                break
        ax.plot(px, py, label=label, lw=2, color=COLORS.get(label))
        # floor (miss at largest cache) for reference
        print(f"{label}: floor(miss@max cache)={ys[-1]:.2f}%, "
              f"miss@{px[0]:.1f}GB={py[0]:.2f}%")

    ax.set_xlim(0, xmax_gb)
    ax.set_ylim(0, 100)
    ticks = []
    t = 0.0
    while t <= xmax_gb + 1e-6:
        ticks.append(round(t)); t += tick_gb
    ax.set_xticks(ticks)
    ax.set_xlabel("Cache size (GB)")
    ax.set_ylabel("Miss rate (%)")
    ax.set_title(f"LRU MRC (zoom 0–{xmax_tb:g} TB, {tick_gb:g} GB grid): dwpd hi / mid / lo")
    ax.grid(True, which="major", alpha=0.4)
    ax.legend(title="trace")
    fig.tight_layout()
    for ext in ("png", "pdf"):
        fig.savefig(f"{out}.{ext}", dpi=130, bbox_inches="tight")
        print(f"wrote {out}.{ext}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
