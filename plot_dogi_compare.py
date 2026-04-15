#!/usr/bin/env python3
import argparse
import glob
import os
import re

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.colors import to_rgb


LINE_RE = re.compile(
    r"^(?P<label>\S+)\s+host_write_bytes=(?P<host>\d+)\s+gc_write_blocks=(?P<gc>\d+)\s+waf=(?P<waf>[0-9.]+).*?host_active=(?P<host_active>[0-9:,]+)\s+gc_active=(?P<gc_active>[0-9:,]+)"
)


def latest_one(pattern: str) -> str:
    matches = sorted(glob.glob(pattern))
    if not matches:
        raise FileNotFoundError(f"no files matched: {pattern}")
    return matches[-1]


def parse_compare_log(path: str):
    rows = []
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            m = LINE_RE.match(line.strip())
            if not m:
                continue
            host_bytes = int(m.group("host"))
            gc_blocks = int(m.group("gc"))
            waf = float(m.group("waf"))
            host_active = parse_counts(m.group("host_active"))
            gc_active = parse_counts(m.group("gc_active"))
            rows.append(
                {
                    "host_bytes": host_bytes,
                    "host_gib": host_bytes / (1024 ** 3),
                    "gc_blocks": gc_blocks,
                    "gc_gib": (gc_blocks * 4096) / (1024 ** 3),
                    "waf": waf,
                    "host_active": host_active,
                    "gc_active": gc_active,
                }
            )
    if not rows:
        raise ValueError(f"no compare-log rows parsed from: {path}")
    return rows


def parse_counts(field: str):
    counts = {}
    for item in field.split(","):
        if not item:
            continue
        idx, value = item.split(":", 1)
        counts[int(idx)] = int(value)
    return counts


def add_series(ax, rows, label, ykey, color=None, linestyle="-", alpha=0.9):
    xs = [r["host_gib"] for r in rows]
    ys = [r[ykey] for r in rows]
    ax.plot(
        xs,
        ys,
        marker="o",
        markersize=3,
        linewidth=1.8,
        label=label,
        color=color,
        linestyle=linestyle,
        alpha=alpha,
    )


def blend_with_white(color, amount):
    r, g, b = to_rgb(color)
    return (
        r + (1.0 - r) * amount,
        g + (1.0 - g) * amount,
        b + (1.0 - b) * amount,
    )


def add_active_series(ax, rows, label, key, idx, color=None, linestyle="-", alpha=0.75):
    xs = [r["host_gib"] for r in rows]
    ys = [r[key].get(idx, 0) for r in rows]
    ax.plot(
        xs,
        ys,
        marker="o",
        markersize=2.5,
        linewidth=1.5,
        label=label,
        color=color,
        linestyle=linestyle,
        alpha=alpha,
    )


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ml", default=None, help="DOGI_ML compare log")
    ap.add_argument("--noml", default=None, help="DOGI_NOML compare log")
    ap.add_argument("--stream", default=None, help="DOGI stream compare log")
    ap.add_argument("--out", default="dogi_compare.png", help="output image path")
    args = ap.parse_args()

    ml_path = args.ml or latest_one("DOGI_ML.dogi_cmp.log.*")
    noml_path = args.noml or latest_one("DOGI_NOML.dogi_cmp.log.*")
    stream_path = args.stream or latest_one("LOG_DOGI_HEURISTIC_88.dogi_cmp.log.*")

    ml_rows = parse_compare_log(ml_path)
    noml_rows = parse_compare_log(noml_path)
    stream_rows = parse_compare_log(stream_path)

    policy_palette = {
        "ml": "#1f77b4",
        "noml": "#d62728",
        "stream": "#2ca02c",
    }
    policy_style = {
        "ml": {"linestyle": "-", "alpha": 0.90},
        "noml": {"linestyle": "--", "alpha": 0.82},
        "stream": {"linestyle": ":", "alpha": 0.76},
    }
    host_tone = {
        0: 0.00,
        1: 0.28,
    }
    gc_tone = {
        0: 0.00,
        1: 0.12,
        2: 0.24,
        3: 0.36,
        4: 0.48,
    }

    fig, axes = plt.subplots(4, 1, figsize=(11, 15), sharex=True)
    ax1, ax2, ax3, ax4 = axes

    add_series(ax1, ml_rows, "DOGI ML", "waf",
               color=policy_palette["ml"], linestyle=policy_style["ml"]["linestyle"], alpha=policy_style["ml"]["alpha"])
    add_series(ax1, noml_rows, "DOGI NO_ML", "waf",
               color=policy_palette["noml"], linestyle=policy_style["noml"]["linestyle"], alpha=policy_style["noml"]["alpha"])
    add_series(ax1, stream_rows, "DOGI stream", "waf",
               color=policy_palette["stream"], linestyle=policy_style["stream"]["linestyle"], alpha=policy_style["stream"]["alpha"])
    ax1.set_ylabel("WAF")
    ax1.set_title("DOGI Compare")
    ax1.grid(True, alpha=0.3)
    ax1.legend()

    add_series(ax2, ml_rows, "DOGI ML", "gc_gib",
               color=policy_palette["ml"], linestyle=policy_style["ml"]["linestyle"], alpha=policy_style["ml"]["alpha"])
    add_series(ax2, noml_rows, "DOGI NO_ML", "gc_gib",
               color=policy_palette["noml"], linestyle=policy_style["noml"]["linestyle"], alpha=policy_style["noml"]["alpha"])
    add_series(ax2, stream_rows, "DOGI stream", "gc_gib",
               color=policy_palette["stream"], linestyle=policy_style["stream"]["linestyle"], alpha=policy_style["stream"]["alpha"])
    ax2.set_xlabel("Host Write (GiB)")
    ax2.set_ylabel("GC Write (GiB)")
    ax2.grid(True, alpha=0.3)

    for idx in range(2):
        add_active_series(
            ax3, ml_rows, f"DOGI ML h{idx}", "host_active", idx,
            color=blend_with_white(policy_palette["ml"], host_tone[idx]),
            linestyle=policy_style["ml"]["linestyle"],
            alpha=policy_style["ml"]["alpha"],
        )
        add_active_series(
            ax3, noml_rows, f"DOGI NO_ML h{idx}", "host_active", idx,
            color=blend_with_white(policy_palette["noml"], host_tone[idx]),
            linestyle=policy_style["noml"]["linestyle"],
            alpha=policy_style["noml"]["alpha"],
        )
        add_active_series(
            ax3, stream_rows, f"DOGI stream h{idx}", "host_active", idx,
            color=blend_with_white(policy_palette["stream"], host_tone[idx]),
            linestyle=policy_style["stream"]["linestyle"],
            alpha=policy_style["stream"]["alpha"],
        )
    ax3.set_ylabel("Host Active Writes")
    ax3.set_title("host_active[0:1]")
    ax3.grid(True, alpha=0.3)
    ax3.legend(ncol=3, fontsize=8)

    for idx in range(5):
        add_active_series(
            ax4, ml_rows, f"DOGI ML g{idx}", "gc_active", idx,
            color=blend_with_white(policy_palette["ml"], gc_tone[idx]),
            linestyle=policy_style["ml"]["linestyle"],
            alpha=policy_style["ml"]["alpha"],
        )
        add_active_series(
            ax4, noml_rows, f"DOGI NO_ML g{idx}", "gc_active", idx,
            color=blend_with_white(policy_palette["noml"], gc_tone[idx]),
            linestyle=policy_style["noml"]["linestyle"],
            alpha=policy_style["noml"]["alpha"],
        )
        add_active_series(
            ax4, stream_rows, f"DOGI stream g{idx}", "gc_active", idx,
            color=blend_with_white(policy_palette["stream"], gc_tone[idx]),
            linestyle=policy_style["stream"]["linestyle"],
            alpha=policy_style["stream"]["alpha"],
        )
    ax4.set_xlabel("Host Write (GiB)")
    ax4.set_ylabel("GC Active Writes")
    ax4.set_title("gc_active[0:4]")
    ax4.grid(True, alpha=0.3)
    ax4.legend(ncol=3, fontsize=8)

    fig.tight_layout()
    fig.savefig(args.out, dpi=160, bbox_inches="tight")

    print(f"saved: {args.out}")
    print(f"ml: {ml_path} ({len(ml_rows)} points)")
    print(f"noml: {noml_path} ({len(noml_rows)} points)")
    print(f"stream: {stream_path} ({len(stream_rows)} points)")


if __name__ == "__main__":
    main()
