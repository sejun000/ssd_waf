#!/usr/bin/env python3
"""
plot_pct_cdf_multi.py – 여러 percentile.txt 파일 → CDF 비교 그래프

입력 형식
  #p  bytes
   1  68608
   ⋯   ⋯
  100 16088974044160

x-축 : gap (bytes, log-scale)
y-축 : CDF (= p / 100)

사용 예
--------
python plot_pct_cdf_multi.py \
       --pct raw_pct.txt,tier_pct.txt,comp_pct.txt \
       --label Raw,Tiering,Compression \
       -o cdf.pdf
"""

import argparse
import itertools
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np
import pandas as pd


def load_percentile(path: Path):
    """percentile 파일 → (x, y)"""
    df = pd.read_csv(path, comment="#", sep=r"\s+", names=["p", "bytes"])
    x = df["bytes"].to_numpy(dtype=np.float64)
    y = df["p"].to_numpy(dtype=np.float64) / 100.0
    return x, y


def main():
    ap = argparse.ArgumentParser(description="Plot CDF from multiple percentile files")
    ap.add_argument(
        "--pct",
        required=True,
        help="comma-separated percentile files (e.g. a.txt,b.txt)",
    )
    ap.add_argument(
        "--label",
        default=None,
        help="comma-separated legend labels (default: file names)",
    )
    ap.add_argument(
        "-o",
        "--output",
        default=None,
        help="output file (pdf/png); omit to display on screen",
    )
    args = ap.parse_args()

    paths = [Path(p.strip()) for p in args.pct.split(",") if p.strip()]
    if not paths:
        ap.error("no percentile files given")

    labels = (
        [s.strip() for s in args.label.split(",")]
        if args.label
        else [p.stem for p in paths]
    )
    if len(labels) < len(paths):
        labels += [p.stem for p in paths[len(labels) :]]

    # ─── 그래프 스타일 설정 ──────────────────────────────────────────────────
    plt.rcParams.update(
        {
            "font.size": 12,
            "axes.titlesize": 14,
            "axes.labelsize": 12,
            "legend.fontsize": 11,
            "xtick.labelsize": 11,
            "ytick.labelsize": 11,
            "lines.linewidth": 1.8,
        }
    )

    # ─── 플롯 ────────────────────────────────────────────────────────────────
    plt.figure(figsize=(6, 4))
    line_styles = itertools.cycle(["-", "--", "-.", ":"])

    for path, lab in zip(paths, labels):
        if not path.is_file():
            print(f"[warn] skip: {path} (not found)")
            continue
        x, y = load_percentile(path)
        plt.plot(x, y, label=lab, linestyle=next(line_styles))

    plt.xscale("log")
    plt.ylim(0, 1)
    plt.xlabel("Inter-write gap (bytes)")
    plt.ylabel("CDF")
    plt.title("Inter-write gap CDF (from percentiles)")
    plt.grid(True, which="both", linestyle=":", linewidth=0.6)
    plt.legend(frameon=False, loc="lower right")
    plt.tight_layout()

    if args.output:
        fmt = "pdf" if args.output.lower().endswith(".pdf") else None
        plt.savefig(args.output, dpi=300, format=fmt)
        print(f"saved → {args.output}")
    else:
        plt.show()


if __name__ == "__main__":
    main()
