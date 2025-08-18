#!/usr/bin/env python3
# plot_distribution.py  (limit‑aware · 4 KiB→GB · memory‑friendly)

import argparse, re, sys, itertools
from pathlib import Path
from collections import Counter
import numpy as np
import matplotlib.pyplot as plt

RE = re.compile(r'reinsert:\s*(\d+)')

BYTES_PER_BLOCK = 4096
GB = 1024 ** 3

# ───────── helpers ───────── #
def count_invalidate(path: Path, limit):
    """파일에서 최대 limit 줄까지 invalidate 값을 세어 반환"""
    cnt = Counter()
    with path.open() as f:
        for line in itertools.islice(f, limit):
            m = RE.search(line)
            if m:
                cnt[int(m.group(1))] += 1

    if not cnt:
        print(f"[warn] {path}: invalidate 값이 없습니다.", file=sys.stderr)

    xs   = np.fromiter(cnt.keys(),   dtype=np.int64)
    freq = np.fromiter(cnt.values(), dtype=np.int64)
    order = np.argsort(xs)
    return xs[order], freq[order]

def to_gigabytes(block_counts: np.ndarray) -> np.ndarray:  # ★ 변경
    """4 KiB 블록 수 → GiB 단위 float 배열"""
    return block_counts * BYTES_PER_BLOCK / GB

def plot_ecdf(ax, xs_gb, freq, label):
    cdf = np.cumsum(freq) / freq.sum()
    ax.step(xs_gb, cdf, where='post', label=label)
    ax.set_ylabel("CDF")

def plot_hist(ax, xs_gb, freq, label, bins='auto'):
    density, bin_edges = np.histogram(xs_gb, bins=bins,
                                      weights=freq, density=True)
    ax.step(bin_edges[:-1], density, where='post', label=label)
    ax.set_ylabel("Density")

# ───────── entry ───────── #
def main():
    p = argparse.ArgumentParser(
        description="Compare invalidate distributions (optionally limited)"
    )
    p.add_argument("--files",  required=True,
                   help="comma‑separated list of files")
    p.add_argument("--labels", required=True,
                   help="comma‑separated list of legend labels")
    p.add_argument("--kind", choices=["ecdf", "hist"], default="ecdf")
    p.add_argument("--bins", type=int, default=200,
                   help="number of bins for hist")
    p.add_argument("--limit", type=int, default=100_000,      # ★ 기본 100k
                   help="max #lines *per file* to scan (default: 100000)")
    p.add_argument("--title", default="Invalidate size distribution (GiB)")
    args = p.parse_args()

    files  = [Path(f.strip()) for f in args.files.split(",")]
    labels = [l.strip()      for l in args.labels.split(",")]
    if len(files) != len(labels):
        p.error("files 와 labels 개수가 달라요!")

    fig, ax = plt.subplots(figsize=(8,5))

    for f, lbl in zip(files, labels):
        xs_blk, freq = count_invalidate(f, args.limit)
        if xs_blk.size == 0:
            continue  # 이미 경고 출력됨
        xs_gb = to_gigabytes(xs_blk)                # ★ 변환
        if args.kind == "ecdf":
            plot_ecdf(ax, xs_gb, freq, lbl)
        else:
            plot_hist(ax, xs_gb, freq, lbl, bins=args.bins)

    ax.set_xlabel("Reinsert Gap size (GiB)")          # ★ 라벨 수정
    ax.set_title(args.title)
    ax.grid(True, ls="--", alpha=0.5)
    ax.legend()
    plt.tight_layout()
    plt.show()

if __name__ == "__main__":
    main()
