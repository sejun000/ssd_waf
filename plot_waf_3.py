#!/usr/bin/env python3
# plot_xy_gib.py
#
# Usage 예)
#   python3 plot_xy_gib.py \
#           --files data1.log,data2.log \
#           --labels FIFO,Greedy \
#           --limit 10000 \
#           --title "Cache size vs. Invalidate" \
#           --out result.png
#
import argparse, sys
from pathlib import Path

import numpy as np
import matplotlib.pyplot as plt

BYTES_IN_GIB = 1024 ** 3


def load_xy(path: Path, limit: int | None = None):
    """첫 두 컬럼을 읽어 GiB 단위 (float64) 배열로 반환."""
    xs, ys = [], []
    with path.open() as f:
        for i, line in enumerate(f):
            if limit is not None and i >= limit:
                break
            if not line.strip():
                continue
            parts = line.split()
            if len(parts) < 2:
                continue
            # byte → GiB
            xs.append(int(parts[0]) / BYTES_IN_GIB)
            ys.append(int(parts[1]) / BYTES_IN_GIB)
    return np.asarray(xs, dtype=np.float64), np.asarray(ys, dtype=np.float64)


def main():
    p = argparse.ArgumentParser(
        description="Plot first‑column vs second‑column (GiB) from multiple files"
    )
    p.add_argument(
        "--files", required=True, help="comma‑separated list of input files"
    )
    p.add_argument(
        "--labels", required=True, help="comma‑separated list of legend labels"
    )
    p.add_argument(
        "--limit", type=int, default=None, help="read at most N lines from each file"
    )
    p.add_argument("--title", default="X vs Y (GiB)")
    p.add_argument(
        "--xscale", choices=["linear", "log"], default="linear", help="x‑axis scale"
    )
    p.add_argument(
        "--yscale", choices=["linear", "log"], default="linear", help="y‑axis scale"
    )
    p.add_argument(
        "--out", help="save figure to this file (omit to show interactively)"
    )
    args = p.parse_args()

    files = [Path(f.strip()) for f in args.files.split(",") if f.strip()]
    labels = [l.strip() for l in args.labels.split(",") if l.strip()]
    if len(files) != len(labels):
        p.error("--files 와 --labels 의 개수가 다릅니다.")

    fig, ax = plt.subplots(figsize=(8, 5))

    for path, label in zip(files, labels):
        x, y = load_xy(path, args.limit)
        if x.size == 0:
            print(f"[warn] {path}: 유효한 데이터가 없습니다.", file=sys.stderr)
            continue
        ax.plot(x, y, marker="o", ls="-", label=label)

    ax.set_xlabel("TLC SSD Writes(GB)")
    ax.set_ylabel("QLC SSD Writes(GB)")
    ax.set_title(args.title)
    ax.set_xscale(args.xscale)
    ax.set_yscale(args.yscale)
    ax.grid(True, ls="--", alpha=0.4)
    ax.legend()
    fig.tight_layout()

    if args.out:
        fig.savefig(args.out, dpi=300)
    else:
        plt.show()


if __name__ == "__main__":
    main()
