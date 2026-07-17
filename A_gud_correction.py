#!/usr/bin/env python3
"""Test inv_rate-based Gud correction.

Hypothesis: ghost under-predicts inv_rate → over-predicts cum_valid → Gud over.
Correction: Gud_corr = Gud × (ghost_inv_rate / real_inv_rate)  (factor < 1)

Three correction variants:
  (A) ORACLE row-by-row: row N's Gud × (ghost_inv_N / real_inv_{N+1}).
      Cheats — uses future info. Shows upper bound of inv_rate correction power.
  (B) CONSTANT per-trace: Gud × (overall ghost_inv_sum / real_inv_sum).
      Trace-level calibration factor.
  (C) EWMA self-correcting: Gud × running_avg(ghost_inv / real_inv).
      Realistic — at row N, use only history up to N. (Skipped for now.)

Compare against raw_compact (col 49) at row N+1, alongside compact_avg baseline.
"""
import sys
from pathlib import Path

def parse(path):
    rows = []
    with open(path) as f:
        next(f)
        for ln in f:
            p = ln.split()
            if len(p) < 63: continue
            try:
                rows.append({
                    "decision":            p[10],
                    "compact_avg":         float(p[22]),       # col 23
                    "compact_evt":         int(float(p[24])),  # col 25
                    "gcsim_m":             float(p[34]),       # col 35
                    "raw_gud":             float(p[47]),       # col 48
                    "raw_compact":         float(p[48]),       # col 49
                    "gud_sum_inv_rate_lf": float(p[60]),       # col 61 (ghost SUM, raw)
                    "real_inv_rate_lf":    float(p[62]),       # col 63 (real MEAN, raw)
                })
            except (ValueError, IndexError):
                continue
    return rows


def analyze(label, path):
    rows = parse(path)
    pairs = []
    for i, r in enumerate(rows):
        if r["decision"] != "RAISE": continue
        if i + 1 >= len(rows): continue
        m = r["gcsim_m"]
        if m <= 0: continue
        K = rows[i+1]["compact_evt"] - r["compact_evt"]
        if K <= 0: continue
        if rows[i+1]["real_inv_rate_lf"] <= 0: continue
        ghost_mean = r["gud_sum_inv_rate_lf"] / m
        real_mean  = rows[i+1]["real_inv_rate_lf"]
        pairs.append({
            "Gud":         r["raw_gud"],
            "compact_avg": r["compact_avg"],
            "actual":      rows[i+1]["raw_compact"],
            "ghost_inv":   ghost_mean,
            "real_inv":    real_mean,
        })
    pairs = pairs[100:]
    if not pairs:
        print(f"\n=== {label} === no pairs"); return None

    # Trace-level constant factor (overall ghost/real ratio)
    sum_g = sum(p["ghost_inv"] for p in pairs)
    sum_r = sum(p["real_inv"]  for p in pairs)
    K_factor = sum_g / sum_r if sum_r > 0 else 1.0
    inv_gap_pct = (1.0 - K_factor) * 100

    print(f"\n=== {label}  ({len(pairs)} pairs) ===")
    print(f"  ghost_inv mean = {sum_g/len(pairs):.5f}, real_inv mean = {sum_r/len(pairs):.5f}")
    print(f"  constant correction factor K = ghost/real = {K_factor:.4f}  (inv gap {inv_gap_pct:+.2f}%)")

    # Predictors:
    #   P0: Gud raw            (baseline ghost predictor)
    #   P1: compact_avg        (naive persistence baseline)
    #   PA: Gud × (ghost_inv_N / real_inv_{N+1})  — ORACLE row-by-row (cheat)
    #   PB: Gud × K_factor                         — CONSTANT per-trace
    for p in pairs:
        ratio = p["ghost_inv"] / p["real_inv"] if p["real_inv"] > 0 else 1.0
        p["Gud_oracle"]   = p["Gud"] * ratio
        p["Gud_constant"] = p["Gud"] * K_factor

    def stats(pkey):
        n = len(pairs)
        sp = sum(p[pkey] for p in pairs)
        sa = sum(p["actual"] for p in pairs)
        mp, ma = sp/n, sa/n
        over = (sp/sa - 1.0)*100 if sa > 0 else 0.0
        mae = sum(abs(p[pkey]-p["actual"]) for p in pairs)/n
        bias = sum(p[pkey]-p["actual"] for p in pairs)/n
        # Pearson r
        num = sum((p[pkey]-mp)*(p["actual"]-ma) for p in pairs)
        dp = (sum((p[pkey]-mp)**2 for p in pairs))**0.5
        da = (sum((p["actual"]-ma)**2 for p in pairs))**0.5
        rr = num/(dp*da) if (dp>0 and da>0) else 0.0
        return (n, mp, ma, over, mae, bias, rr)

    print(f"\n  {'predictor':<26}{'n':>6}{'mean_p':>10}{'mean_a':>10}{'over%':>9}{'MAE':>10}{'bias':>10}{'r':>8}")
    print("  " + "-"*89)
    for name, key in [
        ("Gud (baseline)",      "Gud"),
        ("compact_avg (naive)", "compact_avg"),
        ("Gud × oracle ratio",  "Gud_oracle"),
        ("Gud × const K",       "Gud_constant"),
    ]:
        n, mp, ma, over, mae, bias, rr = stats(key)
        print(f"  {name:<26}{n:>6d}{mp:>10.4f}{ma:>10.4f}{over:>+9.2f}{mae:>10.4f}{bias:>+10.4f}{rr:>8.3f}")


def main():
    traces = [
        ("dwpd01to1",   "/home/sejun000/ssd_waf/GS_FINAL_realinv_v3_dwpd01to1_pr864.gsdec.log"),
        ("dwpd1to2_4x", "/home/sejun000/ssd_waf/GS_FINAL_realinv_v3_dwpd1to2_pr864.gsdec.log"),
        ("dwpd2_5x",    "/home/sejun000/ssd_waf/GS_FINAL_realinv_v3_dwpd2_5x_pr864.gsdec.log"),
    ]
    for name, p in traces:
        if Path(p).exists():
            analyze(name, p)


if __name__ == "__main__":
    main()
