#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
MRC scatter (miss-grid resampling):
x = Interval (bytes/GB/TB)
y = Cache Size (% of device)          ← device_size는 바이트 그대로 사용
color = Miss Rate (%)

동작:
- CSV의 원 점들은 사용하지 않음.
- Interval별로 (CachePct ↔ Miss%) 관계를 정리한 뒤,
  Miss=0..100%를 miss_step 간격으로 잡아, 해당 Miss에 필요한 CachePct를 '선형 보간'으로 계산해 점 생성.
  (관측 범위 밖은 '클램프' 처리: 최소/최대 구간 값으로 고정)

사용 예:
  python mrc_plot_pct.py --file msr_lru.csv \
    --device_size 384136297216 --xunit GB \
    --miss_step 2 --marker '.' --marker_size 16 --edge none \
    --alpha 1.0 --no-grid --save mrc_scatter.png
"""

import argparse
import numpy as np
import pandas as pd
import matplotlib.pyplot as plt
import matplotlib.ticker as mticker

def parse_args():
    p = argparse.ArgumentParser(description="Plot MRC scatter (resample at fixed MISS grid per interval).")
    p.add_argument("--file", required=True,
                   help="CSV with columns: IntervalBytes,CacheSize(blocks),MissRate(%)")
    p.add_argument("--block_size", type=int, default=4096,
                   help="Bytes per block for CacheSize(blocks) (default: 4096)")

    # 전체 디바이스 크기(바이트로 직접 사용)
    p.add_argument("--device_size", type=float, required=True,
                   help="Total device size in BYTES (used as-is).")
    # 호환성(계산에는 사용 안함)
    p.add_argument("--device_unit", choices=["bytes","GB","TB"], default="GB",
                   help="Kept for compatibility; IGNORED in calculation.")

    # 축/표시
    p.add_argument("--xunit", choices=["bytes","GB","TB"], default="GB",
                   help="Unit for IntervalBytes on x-axis (default: GB)")
    p.add_argument("--vlines", action="store_true",
                   help="Draw vertical guide lines for each interval")
    p.add_argument("--dpi", type=int, default=220)
    p.add_argument("--save", default="mrc_scatter.png")

    # 리샘플 간격: Miss=0..100%를 이 간격으로 찍음
    p.add_argument("--miss_step", type=float, default=2.0,
                   help="MISS step in percent for grid: e.g., 2 → Miss=0,2,...,100. "
                        "Use 0 to keep original points (no resampling).")

    # 마커/스타일
    p.add_argument("--marker_size", type=float, default=16.0, help="Marker size (default: 16)  ['.'에 적당]")
    p.add_argument("--marker", default=".", help="Marker: ','(pixel), '.'(dot), 'o', ... (default: '.')")
    p.add_argument("--edge", default="none", help="Marker edgecolor (e.g., 'none','k'; default: none)")
    p.add_argument("--alpha", type=float, default=1.0, help="Marker alpha (default: 1.0)")
    p.add_argument("--x_jitter", type=float, default=0.0,
                   help="Fraction of median interval spacing used as jitter (0 = off)")
    p.add_argument("--no_grid", action="store_true", help="Disable grid")
    p.add_argument("--seed", type=int, default=0, help="Random seed for jitter")
    return p.parse_args()

def unit_divisor(u: str) -> float:
    if u == "bytes": return 1.0
    if u == "GB":    return 1e9
    if u == "TB":    return 1e12
    return 1.0

def make_miss_grid(step: float) -> np.ndarray:
    """0..100을 step 간격으로 포함하도록 생성 (예: step=2 → 0,2,...,100)."""
    step = float(step)
    if step <= 0:
        # 호출자가 0을 주면 리샘플링 하지 않고 원 데이터 사용
        return None
    # floating 오차 방지용 여유 포함
    grid = np.arange(0.0, 100.0 + 1e-9, step, dtype=float)
    # 끝이 100으로 깔끔히 안 떨어지면 100 추가 보증
    if abs(grid[-1] - 100.0) > 1e-9:
        grid = np.append(grid, 100.0)
    return grid

def resample_interval(group: pd.DataFrame, miss_grid: np.ndarray) -> pd.DataFrame:
    """
    한 IntervalX 그룹에서, 주어진 miss_grid (예: 0,2,4,...,100 %)의 각 미스율을
    달성하기 위한 최소 CachePct(디바이스 대비 %)를 '선형 보간'으로 계산해 새 점들을 생성한다.
    - 원시 데이터 점은 사용하지 않고(출력에 포함하지 않음), 보간으로 만든 점만 반환.
    - 절차:
      1) Miss 기준 내림차순 정렬(요청사항)
      2) 같은 Miss가 여러 개면 해당 Miss를 달성하는 최소 CachePct만 유지
      3) np.interp로 miss_grid → CachePct 보간 (xp는 오름차순이어야 하므로 Miss를 오름차순으로 정렬해 사용)
      4) 관측 범위 밖은 클램프(양 끝값으로 고정)
    """
    # 방어 코드
    if group.empty or miss_grid is None or len(miss_grid) == 0:
        return pd.DataFrame(columns=["IntervalX", "CachePct", "Miss"])

    # 1) Miss 내림차순 정렬
    g = group.sort_values("Miss", ascending=False).reset_index(drop=True)

    # 2) 동일 Miss에 대해 최소 CachePct만 유지
    g = g.groupby("Miss", as_index=False)["CachePct"].min()
   # print(group.to_string())
   # print (g.to_string())
   # input()
    # 3) 보간을 위해 Miss 오름차순 정렬 (np.interp는 xp 오름차순 요구)
    x_miss = g["Miss"].to_numpy(dtype=float)       # 미스율(%)
    y_cache = g["CachePct"].to_numpy(dtype=float)  # 해당 미스율 달성 최소 캐시(%)
    idx = np.argsort(x_miss)
    x_miss = x_miss[idx]
    y_cache = y_cache[idx]

    # 단일 점만 있을 때는 상수로 간주
    if x_miss.size == 1:
        cache_interp = np.full_like(miss_grid, y_cache[0], dtype=float)
    else:
        # 4) 선형 보간 + 클램프(좌/우 범위 밖은 끝값으로)
        cache_interp = np.interp(miss_grid, x_miss, y_cache,
                                 left=y_cache[0], right=y_cache[-1])

    return pd.DataFrame({
        "IntervalX": group["IntervalX"].iloc[0],
        "CachePct":  cache_interp,
        "Miss":      miss_grid
    })


def main():
    args = parse_args()
    np.random.seed(args.seed)

    df = pd.read_csv(args.file)
    required = ["IntervalBytes","CacheSize(blocks)","MissRate(%)"]
    missing = [c for c in required if c not in df.columns]
    if missing:
        raise ValueError(f"CSV missing columns: {missing}")
    if args.device_size <= 0:
        raise ValueError("--device_size must be > 0 (bytes)")

    # x축 변환
    interval_x = df["IntervalBytes"].values / unit_divisor(args.xunit)

    # y축: Cache Size (% of device)
    device_bytes = float(args.device_size)  # 곱하지 않음
    cache_bytes  = df["CacheSize(blocks)"].values.astype(np.float64) * float(args.block_size)
    y_pct = (cache_bytes / device_bytes) * 100.0
    y_pct = np.clip(y_pct, 0.0, 100.0)

    # 색: Miss Rate (%)
    miss = df["MissRate(%)"].values.astype(np.float64)

    base_df = pd.DataFrame({
        "IntervalX": interval_x,
        "CachePct":  y_pct,
        "Miss":      miss
    })

    # ★ 리샘플: 각 interval에서 Miss=0..100 step 간격으로 보간 점 생성
    miss_grid = make_miss_grid(args.miss_step)
    if miss_grid is not None:
        groups = []
        for xval, g in base_df.groupby("IntervalX", sort=True):
            #print (xval)
            #print (g)
            # press any key
            #input()
            g = g.copy()
            g.loc[:, "IntervalX"] = xval  # 안정성
            groups.append(resample_interval(g, miss_grid))
        plot_df = pd.concat(groups, ignore_index=True) if groups else base_df
    else:
        # miss_step=0이면 원 데이터 사용
        plot_df = base_df

    # 필요 시: 아주 약한 x 지터(겹침 해소; 값 자체는 그대로)
    """
    if args.x_jitter > 0 and plot_df["IntervalX"].nunique() >= 2:
        uniq = np.sort(plot_df["IntervalX"].unique())
        step = float(np.median(np.diff(uniq))) if len(uniq) > 1 else 1.0
        width = args.x_jitter * step
        noise = (np.random.rand(len(plot_df)) - 0.5) * 2.0 * width
        plot_df = plot_df.assign(IntervalX=plot_df["IntervalX"] + noise)
    """

    # 그리기(순수 scatter)
    fig = plt.figure(figsize=(11, 6.5))
    ax  = plt.gca()

    if args.vlines:
        for xv in sorted(plot_df["IntervalX"].unique()):
            ax.axvline(x=xv, linestyle="--", linewidth=0.7, alpha=0.35)

    sc = ax.scatter(
        plot_df["IntervalX"], plot_df["CachePct"],
        c=plot_df["Miss"], cmap="coolwarm",
        s=args.marker_size, marker=args.marker,
        alpha=args.alpha, edgecolors=args.edge
    )
    cbar = plt.colorbar(sc, ax=ax)
    cbar.set_label("Miss Rate (%)")

    ax.set_title("Miss Rate (color) over Interval vs Cache Size (% of device)", fontsize=14)
    ax.set_xlabel(f"Write Bytes ({args.xunit})", fontsize=12)
    ax.set_ylabel("Cache Size (% of device)", fontsize=12)
    ax.set_ylim(0, 100)
    ax.yaxis.set_major_locator(mticker.MultipleLocator(10))
    ax.yaxis.set_minor_locator(mticker.MultipleLocator(5))
    if not args.no_grid:
        ax.grid(True, which="both", linestyle="--", linewidth=0.5)

    # x padding
    if len(plot_df):
        xmin, xmax = float(plot_df["IntervalX"].min()), float(plot_df["IntervalX"].max())
        pad = 0.02 * max(1e-9, (xmax - xmin) if xmax > xmin else (xmax if xmax > 0 else 1.0))
        ax.set_xlim(xmin - pad, xmax + pad)

    plt.tight_layout()
    plt.savefig(args.save, dpi=args.dpi)
    print(f"Saved: {args.save}")

if __name__ == "__main__":
    main()
