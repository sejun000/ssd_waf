#!/usr/bin/env python3
"""Plot survival analysis: age at snapshot → avg time-to-death (GB)."""

import sys
import pandas as pd
import matplotlib.pyplot as plt

BLOCKS_PER_GB = 1024 * 1024 * 1024 / 4096  # 262144

def main():
    csv_path = sys.argv[1] if len(sys.argv) > 1 else "survival_analysis.csv"
    df = pd.read_csv(csv_path)

    # convert blocks → GB
    df["age_mid_gb"] = (df["age_bucket_start"] + df["age_bucket_end"]) / 2 / BLOCKS_PER_GB
    df["avg_ttd_invalidate_gb"] = df["avg_ttd_invalidate"] / BLOCKS_PER_GB
    df["avg_ttd_evict_gb"]      = df["avg_ttd_evict"] / BLOCKS_PER_GB
    df["avg_ttd_all_gb"]        = df["avg_ttd_all"] / BLOCKS_PER_GB
    bucket_width_gb = (df["age_bucket_end"].iloc[0] - df["age_bucket_start"].iloc[0]) / BLOCKS_PER_GB

    fig, axes = plt.subplots(2, 1, figsize=(12, 10), sharex=True)

    # ── Top: Average TTD by age ──
    ax = axes[0]
    mask_inv = df["count_invalidate"] > 0
    mask_ev  = df["count_evict"] > 0
    mask_all = df["count_all"] > 0

    if mask_all.any():
        ax.plot(df.loc[mask_all, "age_mid_gb"],
                df.loc[mask_all, "avg_ttd_all_gb"],
                "o-", label="All (invalidate + evict)", color="black", markersize=3)
    if mask_inv.any():
        ax.plot(df.loc[mask_inv, "age_mid_gb"],
                df.loc[mask_inv, "avg_ttd_invalidate_gb"],
                "s--", label="Host invalidate (overwrite)", color="tab:blue", markersize=3)
    if mask_ev.any():
        ax.plot(df.loc[mask_ev, "age_mid_gb"],
                df.loc[mask_ev, "avg_ttd_evict_gb"],
                "^--", label="Evict (flush)", color="tab:red", markersize=3)

    ax.set_ylabel("Avg Time-To-Death (GB)")
    ax.set_title("Survival Analysis: Block Age at Snapshot → Avg Time-To-Death")
    ax.legend()
    ax.grid(True, alpha=0.3)

    # ── Bottom: Count per bucket ──
    ax2 = axes[1]
    ax2.bar(df["age_mid_gb"], df["count_invalidate"],
            width=bucket_width_gb, alpha=0.5, label="Invalidated", color="tab:blue")
    ax2.bar(df["age_mid_gb"], df["count_evict"],
            width=bucket_width_gb, alpha=0.5, bottom=df["count_invalidate"],
            label="Evicted", color="tab:red")
    ax2.bar(df["age_mid_gb"], df["still_alive"],
            width=bucket_width_gb, alpha=0.3,
            bottom=df["count_invalidate"] + df["count_evict"],
            label="Still alive", color="tab:green")

    ax2.set_xlabel("Block Age at Snapshot (GB)")
    ax2.set_ylabel("Count")
    ax2.legend()
    ax2.grid(True, alpha=0.3)

    plt.tight_layout()
    out = csv_path.replace(".csv", ".png")
    plt.savefig(out, dpi=150)
    print(f"Saved to {out}")

if __name__ == "__main__":
    main()
