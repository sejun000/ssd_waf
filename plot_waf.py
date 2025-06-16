#!/usr/bin/env python3
"""
plot_waf_multi.py – 여러 trace의 WAF(ΔNAND/ΔHOST) 꺾은선 그래프

입력 CSV 형식
  col0(무시), col1 = NAND-write bytes (누적), col2 = HOST-write bytes (누적)

x-축 : 누적 HOST-write (bytes)
y-축 : ΔNAND / ΔHOST  (각 행과 직전 행 차분)

사용 예
  python plot_waf_multi.py \
         --csv raw.csv,tier.csv,hybrid.csv \
         --label Raw,Tiering,Hybrid \
         -o waf.pdf
"""

import argparse
import itertools
from pathlib import Path

import matplotlib.pyplot as plt
import pandas as pd


def load_waf(path: Path):
    """CSV 한 개 → (host array, waf array)"""
    df = pd.read_csv(path, header=None, names=["_", "nand", "host"])
    dn = df["nand"].diff()
    dh = df["host"].diff()
    waf = dn / dh
    # 첫 행 NaN 제거
    return df["host"].iloc[1:].to_numpy(), waf.iloc[1:].to_numpy()


def main():
    ap = argparse.ArgumentParser(
        description="Plot WAF vs host-write for multiple traces"
    )
    ap.add_argument(
        "--csv",
        required=True,
        help="comma-separated CSV file list (e.g. a.csv,b.csv,c.csv)",
    )
    ap.add_argument(
        "--label",
        default=None,
        help="comma-separated label list (default: file names)",
    )
    ap.add_argument(
        "-o",
        "--output",
        default=None,
        help="PDF/PNG file to save (omit to show on screen)",
    )
    args = ap.parse_args()

    paths = [Path(p.strip()) for p in args.csv.split(",") if p.strip()]
    if not paths:
        ap.error("no CSV files given")

    labels = (
        [s.strip() for s in args.label.split(",")]
        if args.label
        else [p.stem for p in paths]
    )
    # label 개수가 파일보다 적으면 뒤쪽은 파일 이름으로 채움
    if len(labels) < len(paths):
        labels += [p.stem for p in paths[len(labels) :]]

    # ─── plot ───────────────────────────────────────────────────────────────
    plt.figure(figsize=(6.5, 4))
    line_cycler = itertools.cycle(
        [
            {"linestyle": "-"},
            {"linestyle": "--"},
            {"linestyle": "-."},
            {"linestyle": ":"},
        ]
    )

    for path, lab in zip(paths, labels):
        if not path.is_file():
            print(f"[warn] skip: {path} (not found)")
            continue
        x, y = load_waf(path)
        plt.plot(x, y, label=lab, **next(line_cycler))

    plt.xlabel("Host-write bytes")
    plt.ylabel("WAF  (ΔNAND / ΔHOST)")
    plt.title("Write-Amplification vs Host-write")
    plt.grid(True, linewidth=0.6, linestyle=":")
    plt.legend(frameon=False)
    plt.tight_layout()

    if args.output:
        fmt = "pdf" if args.output.lower().endswith(".pdf") else None
        plt.savefig(args.output, dpi=300, format=fmt)
        print(f"saved → {args.output}")
    else:
        plt.show()


if __name__ == "__main__":
    main()
