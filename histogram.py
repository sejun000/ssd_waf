#!/usr/bin/env python3
"""
plot_summary.py – flexible log summary visualizer
=================================================
• 여러 로그 파일의 마지막 summary 부분을 파싱하여 히스토그램(막대그래프) 생성.
• --histogram_metric 으로 특정 summary 섹션을 지정.
• --files 와 --labels 로 여러 파일을 비교, 각각의 파일은 2열 그리드(grid)에 그려짐.
• --rank 로 지정된 모든 순위에 대해 데이터가 없으면 0으로 표시.

usage example
-------------
python plot_summary.py \
    --files run1.log,run2.log,run3.log \
    --labels FIFO,Greedy,LFU \
    --histogram_metric evicted_blocks \
    --rank 20 \
    --output evicted_blocks_summary.png
"""
import argparse
import re
from pathlib import Path
import sys
import math # subplot 행 계산을 위해 math 라이브러리 추가

import matplotlib.pyplot as plt
import numpy as np

# ────────────────────────────────────────────────────────────────────────────────

def parse_summary(path: Path, metric_name: str) -> list[tuple[int, int]]:
    """지정된 로그 파일에서 특정 메트릭의 summary 데이터를 파싱합니다."""
    data = []
    in_target_summary = False
    line_re = re.compile(r"(\d+)\s+th\s+(\d+)")

    with path.open('r', encoding='utf-8') as f:
        for line in f:
            stripped_line = line.strip()
            if stripped_line == f"---summary of {metric_name}---":
                in_target_summary = True
                continue

            if in_target_summary:
                if stripped_line.startswith("---"):
                    break
                match = line_re.match(stripped_line)
                if match:
                    rank = int(match.group(1))
                    value = int(match.group(2))
                    data.append((rank, value))
    
    data.sort()
    return data

# ────────────────────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(description="Plot histograms from log summary sections.")
    ap.add_argument("--files", required=True, help="쉼표로 구분된 로그 파일 경로")
    ap.add_argument("--labels", help="쉼표로 구분된 각 파일의 레이블 (그래프 제목에 사용)")
    ap.add_argument("--histogram_metric", required=True, help="summary에서 사용할 메트릭 이름 (e.g., evicted_blocks)")
    ap.add_argument("--rank", type=int, default=20, help="출력할 상위 순위 개수 (기본값: 20, 0th~19th)")
    ap.add_argument("--ylabel", default="Value", help="Y축의 라벨 지정")
    ap.add_argument("--log", action="store_true", help="Y축을 로그 스케일로 설정")
    ap.add_argument("--output", default="summary_histogram.png", help="저장할 PNG 파일 이름")
    args = ap.parse_args()

    files = [Path(p.strip()) for p in args.files.split(",")]
    
    if args.labels:
        labels = [l.strip() for l in args.labels.split(",")]
    else:
        labels = [fp.name for fp in files]

    if len(files) != len(labels):
        ap.error("--files 와 --labels 의 개수가 일치해야 합니다.")

    # ======================================================================
    # ▼▼▼▼▼▼▼▼▼▼▼▼▼▼▼▼▼▼▼▼▼▼▼▼▼▼▼▼▼▼▼▼▼▼▼▼▼▼▼▼▼▼▼▼▼▼▼▼▼▼▼▼▼▼▼▼▼▼▼▼▼▼▼▼▼▼▼▼▼
    # 변경된 로직: subplot을 2열로 생성
    # ======================================================================
    num_files = len(files)
    ncols = 2
    # 파일 개수에 따라 필요한 행(row)의 수를 계산 (예: 3개 파일 -> 2행 필요)
    nrows = math.ceil(num_files / ncols)
    
    # figsize의 가로를 늘리고, 세로는 행의 수에 비례하도록 조정
    fig, axes = plt.subplots(nrows=nrows, ncols=ncols, figsize=(20, 6 * nrows), squeeze=False)
    axes = axes.flatten() # 2D 배열을 1D로 만들어 순회하기 쉽게 함

    for i, (fp, lbl) in enumerate(zip(files, labels)):
        ax = axes[i]
        summary_data = parse_summary(fp, args.histogram_metric)

        if not summary_data:
            print(f"[경고] {fp.name} 파일에서 '{args.histogram_metric}' summary를 찾을 수 없습니다.", file=sys.stderr)
            ax.text(0.5, 0.5, f"'{args.histogram_metric}' 데이터를 찾을 수 없음", ha='center', va='center')
            ax.set_title(f"{lbl}: {args.histogram_metric}", fontsize=24) # <-- title 크기 변경
            continue

        data_dict = dict(summary_data)
        all_ranks = list(range(args.rank))
        all_values = [data_dict.get(r, 0) for r in all_ranks]
        
        ranks = all_ranks
        values = all_values
        
        xticklabels = [f"{r} th" for r in ranks]

        ax.bar(xticklabels, values, color='skyblue', edgecolor='black')
        
        # <<<--- 주요 변경점: fontsize를 24로 수정 --->>>
        ax.set_title(f"'{lbl}' - Summary of {args.histogram_metric}", fontsize=24)
        
        ax.set_ylabel(args.ylabel)
        ax.tick_params(axis='x', rotation=45, labelsize=10)

        if args.log:
            ax.set_yscale('log')

        ax.grid(True, axis='y', linestyle='--', alpha=0.7)

    # 파일 개수가 홀수일 때 등, 비어있는 subplot을 보이지 않게 처리
    for i in range(num_files, len(axes)):
        axes[i].set_visible(False)
    
    # ▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲
    # ======================================================================

    plt.tight_layout(pad=3.0)
    plt.savefig(args.output)
    print(f"📊 히스토그램이 {args.output} 파일로 저장되었습니다.")

if __name__ == "__main__":
    main()