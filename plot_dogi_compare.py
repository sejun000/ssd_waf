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
    r"^(?P<label>\S+)\s+host_write_bytes=(?P<host>\d+)\s+gc_write_blocks=(?P<gc>\d+)\s+waf=(?P<waf>[0-9.]+).*?host_active=(?P<host_active>[0-9:,]+)\s+gc_active=(?P<gc_active>[0-9:,]+)(?:.*?host_age_bucket=(?P<host_age_bucket>[0-9:,]+)\s+host_est_bucket=(?P<host_est_bucket>[0-9:,]+))?"
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
            host_age_bucket = parse_counts(m.group("host_age_bucket")) if m.group("host_age_bucket") else {}
            host_est_bucket = parse_counts(m.group("host_est_bucket")) if m.group("host_est_bucket") else {}
            rows.append(
                {
                    "host_bytes": host_bytes,
                    "host_gib": host_bytes / (1024 ** 3),
                    "gc_blocks": gc_blocks,
                    "gc_gib": (gc_blocks * 4096) / (1024 ** 3),
                    "waf": waf,
                    "host_active": host_active,
                    "gc_active": gc_active,
                    "host_age_bucket": host_age_bucket,
                    "host_est_bucket": host_est_bucket,
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


def add_active_series_mapped(ax, rows, label, key, idx_map, color=None, linestyle="-", alpha=0.75):
    xs = [r["host_gib"] for r in rows]
    ys = [r[key].get(src_idx, 0) for r, src_idx in zip(rows, idx_map)]
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


def add_bucket_bars(ax, datasets, key, title, ylabel):
    bucket_labels = ["0", "1", "2", "3", "4", "5", "6", "7", "8-15", "16-31", "32-63", "64+"]
    xs = list(range(len(bucket_labels)))
    width = 0.24
    offsets = [-width, 0.0, width]

    for (label, rows, color), offset in zip(datasets, offsets):
        if not rows:
            continue
        counts = rows[-1].get(key, {})
        total = sum(counts.values())
        if total == 0:
            continue
        ys = [counts.get(i, 0) / total for i in range(len(bucket_labels))]
        ax.bar([x + offset for x in xs], ys, width=width, label=label, color=color, alpha=0.8)

    ax.set_xticks(xs)
    ax.set_xticklabels(bucket_labels)
    ax.set_ylabel(ylabel)
    ax.set_title(title)
    ax.grid(True, axis="y", alpha=0.3)
    ax.legend()


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

    fig, axes = plt.subplots(6, 1, figsize=(12, 21), sharex=False)
    ax1, ax2, ax3, ax4, ax5, ax6 = axes

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
            ax4, ml_rows, f"DOGI ML g{idx}", "gc_active", idx + 1,
            color=blend_with_white(policy_palette["ml"], gc_tone[idx]),
            linestyle=policy_style["ml"]["linestyle"],
            alpha=policy_style["ml"]["alpha"],
        )
        add_active_series(
            ax4, noml_rows, f"DOGI NO_ML g{idx}", "gc_active", idx + 1,
            color=blend_with_white(policy_palette["noml"], gc_tone[idx]),
            linestyle=policy_style["noml"]["linestyle"],
            alpha=policy_style["noml"]["alpha"],
        )
        add_active_series(
            ax4, stream_rows, f"DOGI stream g{idx}", "gc_active", idx + 11,
            color=blend_with_white(policy_palette["stream"], gc_tone[idx]),
            linestyle=policy_style["stream"]["linestyle"],
            alpha=policy_style["stream"]["alpha"],
        )
    ax4.set_xlabel("Host Write (GiB)")
    ax4.set_ylabel("GC Active Writes")
    ax4.set_title("gc_active stages 0:4 (DOGI=1:5, stream=11:15)")
    ax4.grid(True, alpha=0.3)
    ax4.legend(ncol=3, fontsize=8)

    bucket_datasets = [
        ("DOGI ML", ml_rows, policy_palette["ml"]),
        ("DOGI NO_ML", noml_rows, policy_palette["noml"]),
        ("DOGI stream", stream_rows, policy_palette["stream"]),
    ]
    add_bucket_bars(ax5, bucket_datasets, "host_age_bucket", "host_age_bucket (final row ratio)", "Ratio")
    add_bucket_bars(ax6, bucket_datasets, "host_est_bucket", "host_est_bucket (final row ratio)", "Ratio")
    ax6.set_xlabel("Bucket")

    fig.tight_layout()
    fig.savefig(args.out, dpi=160, bbox_inches="tight")

    print(f"saved: {args.out}")
    print(f"ml: {ml_path} ({len(ml_rows)} points)")
    print(f"noml: {noml_path} ({len(noml_rows)} points)")
    print(f"stream: {stream_path} ({len(stream_rows)} points)")


if __name__ == "__main__":
    main()
