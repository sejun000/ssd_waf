#!/usr/bin/env python3
"""
plot_multi.py  –  여러 파일을 한 그래프에 그려 비교한다.

usage:  plot_multi.py --file f1.txt,f2.txt --col 1,4 [--waf]
                      [--legend L1,L2] [--xlabel XLABEL] [--ylabel YLABEL]

• --file     : 콤마(,)로 구분한 입력 파일 목록
• --col      : 가로축·세로축으로 쓸 두 개의 1-기반 컬럼 번호 (예: 1,4)
• --waf      : y = (Δcol_b)/(Δcol_a)  로 계산
• --legend   : 콤마(,)로 구분한 레전드(파일 수와 동일해야 함)
• --xlabel   : x축 라벨 (생략 시 자동)
• --ylabel   : y축 라벨 (생략 시 자동)
"""

import argparse
from pathlib import Path
import sys

import matplotlib.pyplot as plt
import numpy as np

# ────────────────────────────── 유틸 ──────────────────────────────
def split_csv(arg: str) -> list[str]:
    """쉼표로 구분한 인자를 공백 제거해 리스트로 반환"""
    return [s.strip() for s in arg.split(",") if s.strip()]

def load_columns(path: str | Path, cols: tuple[int, int]) -> tuple[np.ndarray, np.ndarray]:
    """파일에서 두 컬럼만 읽어 ndarray(x), ndarray(y) 반환"""
    data = np.loadtxt(path, dtype=np.float64)
    x = data[:, cols[0]]
    y = data[:, cols[1]]
    return x, y

# ────────────────────────────── 메인 ──────────────────────────────
def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(formatter_class=argparse.RawDescriptionHelpFormatter,
        description="Plot multiple files with optional WAF derivative.")
    p.add_argument("--file", required=True,
                   help="comma-separated list of input text files")
    p.add_argument("--col", required=True,
                   help="two comma-separated 1-based column numbers, e.g. 1,4")
    p.add_argument("--waf", action="store_true",
                   help="use (Δy)/(Δx) for y-values")
    p.add_argument("--legend",
                   help="comma-separated legend labels (must match number of files)")
    p.add_argument("--xlabel", help="x-axis label")
    p.add_argument("--ylabel", help="y-axis label")
    return p.parse_args()

def main() -> None:
    args = parse_args()

    files = split_csv(args.file)
    if not files:
        sys.exit("No input files given with --file")

    a, b = (int(c.strip()) - 1 for c in args.col.split(","))
    if a < 0 or b < 0:
        sys.exit("--col indices must be ≥ 1")

    # 레전드 처리
    legends = split_csv(args.legend) if args.legend else []
    if legends and len(legends) != len(files):
        sys.exit("Number of legends must match number of files")

    # 색상/스타일 순환
    prop_cycle = plt.rcParams['axes.prop_cycle']
    style_iter = iter(prop_cycle())

    for idx, fpath in enumerate(files):
        x, y = load_columns(fpath, (a, b))

        if args.waf:
            if len(x) < 2:
                sys.exit(f"{fpath}: need ≥2 rows for WAF calculation")
            dx = np.diff(x)
            dy = np.diff(y)
            y_plot = dy / dx
            x_plot = (x[:-1] + x[1:]) / 2  # 구간 평균
        else:
            x_plot, y_plot = x, y

        style = next(style_iter)
        label = legends[idx] if legends else Path(fpath).stem
        plt.plot(
            x_plot,
            y_plot,
            marker=".",
            linewidth=0.8,   # ← 선 두께(기본 1.5). 작게 할수록 얇아짐
            markersize=1,    # ← 마커 지름(포인트 단위). 작게 할수록 작아짐
            label=label,
            **style
        )

    # 축 라벨
    plt.xlabel(args.xlabel if args.xlabel else f"column {a+1}")
    if args.ylabel:
        plt.ylabel(args.ylabel)
    else:
        plt.ylabel(f"(Δcol{b+1})/(Δcol{a+1})" if args.waf else f"column {b+1}")

    plt.title("WAF comparison" if args.waf else "Nand write comparison")
    plt.grid(True)
    plt.legend()
    plt.tight_layout()
    plt.show()

if __name__ == "__main__":
    main()
