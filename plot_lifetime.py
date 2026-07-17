#!/usr/bin/env python3
"""
Plot mean residual life: no-cache rewrite vs with-cache invalidate+evict.

Reads:
  - lifetime_histogram.csv  (with-cache block lifetimes)
  - rewrite_histogram.csv   (no-cache rewrite intervals)

X-axis: age (GB)
Y-axis: mean residual life (GB)
"""

import sys, os
import numpy as np
import pandas as pd
import matplotlib.pyplot as plt

BLOCKS_PER_GB = 1024 * 1024 * 1024 / 4096  # 262144


def mean_residual_life_with_std(mid, counts):
    """E[T - x | T > x] and StdDev[T - x | T > x] for each midpoint x."""
    n = len(counts)
    if counts.sum() == 0:
        return np.zeros(n), np.zeros(n), np.zeros(n)

    suffix_count  = np.cumsum(counts[::-1])[::-1]
    suffix_sum    = np.cumsum((mid * counts)[::-1])[::-1]
    suffix_sum_sq = np.cumsum((mid**2 * counts)[::-1])[::-1]

    mrl = np.zeros(n)
    std = np.zeros(n)
    valid = suffix_count > 0

    # E[T | T>x]
    mean_T = np.zeros(n)
    mean_T[valid] = suffix_sum[valid] / suffix_count[valid]
    # E[T^2 | T>x]
    mean_T2 = np.zeros(n)
    mean_T2[valid] = suffix_sum_sq[valid] / suffix_count[valid]

    # MRL = E[T|T>x] - x
    mrl[valid] = mean_T[valid] - mid[valid]

    # Var[T-x|T>x] = Var[T|T>x] = E[T^2|T>x] - (E[T|T>x])^2
    var = np.zeros(n)
    var[valid] = mean_T2[valid] - mean_T[valid]**2
    var = np.maximum(var, 0)  # numerical safety
    std = np.sqrt(var)

    return mrl, std, suffix_count


def load_lifetime(path="lifetime_histogram.csv"):
    df = pd.read_csv(path)
    mid = ((df["lifetime_start"] + df["lifetime_end"]) / 2).values
    ci = df["count_invalidate"].values.astype(float)
    ce = df["count_evict"].values.astype(float)
    ca = df["count_all"].values.astype(float)
    bw = df["lifetime_end"].iloc[0] - df["lifetime_start"].iloc[0]
    return mid, ci, ce, ca, bw


def load_rewrite(path="rewrite_histogram.csv"):
    df = pd.read_csv(path)
    mid = ((df["interval_start"] + df["interval_end"]) / 2).values
    cnt = df["count"].values.astype(float)
    bw  = df["interval_end"].iloc[0] - df["interval_start"].iloc[0]
    return mid, cnt, bw


def main():
    lt_path = "lifetime_histogram.csv"
    rw_path = "rewrite_histogram.csv"

    has_lt = os.path.exists(lt_path)
    has_rw = os.path.exists(rw_path)

    if not has_lt and not has_rw:
        print("No CSV files found.")
        return

    fig, ax = plt.subplots(figsize=(14, 7))

    # Determine x-axis limit from cache lifetime (evict spike ≈ cache size)
    xlim = None

    # ── With-cache MRL ──
    if has_lt:
        lt_mid, ci, ce, ca, lt_bw = load_lifetime(lt_path)
        lt_mid_gb = lt_mid / BLOCKS_PER_GB

        mrl_all, std_all, sc_all = mean_residual_life_with_std(lt_mid, ca)
        mask = sc_all > 100

        if mask.any():
            x = lt_mid_gb[mask]
            y = mrl_all[mask] / BLOCKS_PER_GB
            y_std = std_all[mask] / BLOCKS_PER_GB

            ax.plot(x, y, "-", label="With-cache MRL", color="black", linewidth=2)
            ax.fill_between(x, y - y_std, y + y_std,
                            alpha=0.15, color="tab:blue", label="With-cache +/- 1 Std Dev")
            xlim = x[-1]  # max age in cache lifetime data

    # ── No-cache rewrite MRL (clipped to cache age range) ──
    if has_rw:
        rw_mid, rw_cnt, rw_bw = load_rewrite(rw_path)
        rw_mid_gb = rw_mid / BLOCKS_PER_GB

        rw_mrl, rw_std, rw_sc = mean_residual_life_with_std(rw_mid, rw_cnt)
        mask = rw_sc > 100

        if mask.any():
            x = rw_mid_gb[mask]
            y = rw_mrl[mask] / BLOCKS_PER_GB
            y_std = rw_std[mask] / BLOCKS_PER_GB

            # clip to cache age range
            if xlim is not None:
                clip = x <= xlim
                x, y, y_std = x[clip], y[clip], y_std[clip]

            ax.plot(x, y, "-", label="No-cache rewrite MRL", color="tab:green", linewidth=2)
            ax.fill_between(x, y - y_std, y + y_std,
                            alpha=0.15, color="tab:green", label="No-cache +/- 1 Std Dev")

    if xlim is not None:
        ax.set_xlim(0, xlim)

    ax.set_xlabel("Age (GB)")
    ax.set_ylabel("Mean Residual Life (GB)")
    ax.set_title("Mean Residual Life: With-Cache vs No-Cache Rewrite")
    ax.legend()
    ax.grid(True, alpha=0.3)

    plt.tight_layout()
    out = "mrl_comparison.png"
    plt.savefig(out, dpi=150)
    print(f"Saved to {out}")


if __name__ == "__main__":
    main()
