#!/usr/bin/env python3
"""SUM comparison: ghost sum (col 60) vs real sum (col 62 × K).

K = real compaction event count during the 1-seg period
  = compact_evt[N+1] - compact_evt[N]  (col 25 is cumulative)

ghost m (col 35) is the average ghost-picked victim count per scan
(to free 1 seg, fractional last).

If m ≈ K → ghost sum and real sum are on the same scale → direct comparison.
If m ≠ K → unit mismatch; sum diff reflects both rate diff AND count diff.
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
                    "decision":              p[10],
                    "compact_evt":           int(float(p[24])),     # col 25
                    "gcsim_m":               float(p[34]),          # col 35
                    "raw_gud":               float(p[47]),
                    "raw_compact":           float(p[48]),
                    "gud_sum_inv_rate":      float(p[59]),          # col 60 (SUM)
                    "gud_sum_inv_rate_lf":   float(p[60]),          # col 61 (SUM)
                    "real_inv_rate":         float(p[61]),          # col 62 (MEAN)
                    "real_inv_rate_lf":      float(p[62]),          # col 63 (MEAN)
                })
            except (ValueError, IndexError):
                continue
    return rows


def build_pairs(rows):
    """Row N (RAISE) ghost → Row N+1 real, with K = compact_evt diff."""
    pairs = []
    for i, r in enumerate(rows):
        if r["decision"] != "RAISE": continue
        if i + 1 >= len(rows): continue
        m = r["gcsim_m"]
        if m <= 0: continue
        K = rows[i+1]["compact_evt"] - r["compact_evt"]   # # real compactions in period
        if K <= 0: continue   # no real compaction in this period

        ghost_sum_ewma = r["gud_sum_inv_rate"]               # already SUM
        ghost_sum_raw  = r["gud_sum_inv_rate_lf"]            # already SUM
        real_sum_ewma  = rows[i+1]["real_inv_rate"]    * K   # MEAN × K = SUM
        real_sum_raw   = rows[i+1]["real_inv_rate_lf"] * K   # MEAN × K = SUM

        pairs.append({
            "m": m, "K": K,
            "ghost_sum_ewma": ghost_sum_ewma,
            "ghost_sum_raw":  ghost_sum_raw,
            "real_sum_ewma":  real_sum_ewma,
            "real_sum_raw":   real_sum_raw,
            "ghost_mean_ewma": ghost_sum_ewma / m,
            "ghost_mean_raw":  ghost_sum_raw  / m,
            "real_mean_ewma":  real_sum_ewma  / K,
            "real_mean_raw":   real_sum_raw   / K,
        })
    return pairs[100:]


def stats(pairs, pkey, akey):
    n = len(pairs)
    if n == 0: return None
    sp = sum(p[pkey] for p in pairs)
    sa = sum(p[akey] for p in pairs)
    mp, ma = sp/n, sa/n
    over = (sp/sa - 1.0)*100 if sa > 0 else 0.0
    mae = sum(abs(p[pkey]-p[akey]) for p in pairs)/n
    num = sum((p[pkey]-mp)*(p[akey]-ma) for p in pairs)
    dp = (sum((p[pkey]-mp)**2 for p in pairs))**0.5
    da = (sum((p[akey]-ma)**2 for p in pairs))**0.5
    r = num/(dp*da) if (dp>0 and da>0) else 0.0
    return (n, mp, ma, over, mae, r)


def analyze(label, path):
    rows = parse(path)
    pairs = build_pairs(rows)
    if not pairs:
        print(f"\n=== {label} === no pairs"); return
    print(f"\n=== {label}  ({len(pairs)} pairs) ===")

    # m vs K
    m_avg = sum(p["m"] for p in pairs)/len(pairs)
    K_avg = sum(p["K"] for p in pairs)/len(pairs)
    print(f"  avg ghost m = {m_avg:.3f},  avg real K = {K_avg:.3f},  K/m = {K_avg/m_avg:.3f}")

    print(f"\n  {'comparison':<38}{'n':>6}{'mean_g':>11}{'mean_r':>11}{'over%':>9}{'MAE':>11}{'r':>8}")
    print("  " + "-"*94)
    for tag, pkey, akey in [
        ("MEAN (ghost_sum/m vs real_sum/K)",   "ghost_mean_raw",  "real_mean_raw"),
        ("SUM  (ghost_sum    vs real_sum)",    "ghost_sum_raw",   "real_sum_raw"),
        ("MEAN-EWMA",                          "ghost_mean_ewma", "real_mean_ewma"),
        ("SUM-EWMA",                           "ghost_sum_ewma",  "real_sum_ewma"),
    ]:
        s = stats(pairs, pkey, akey)
        if s is None: continue
        n, mp, ma, over, mae, r = s
        print(f"  {tag:<38}{n:>6d}{mp:>11.5f}{ma:>11.5f}{over:>+9.2f}{mae:>11.5f}{r:>8.3f}")


def main():
    traces = [
        ("dwpd01to1 (v3)",   "/home/sejun000/ssd_waf/GS_FINAL_realinv_v3_dwpd01to1_pr864.gsdec.log"),
        ("dwpd1to2_4x (v3)", "/home/sejun000/ssd_waf/GS_FINAL_realinv_v3_dwpd1to2_pr864.gsdec.log"),
        ("dwpd2_5x (v3)",    "/home/sejun000/ssd_waf/GS_FINAL_realinv_v3_dwpd2_5x_pr864.gsdec.log"),
    ]
    for name, p in traces:
        if Path(p).exists():
            analyze(name, p)


if __name__ == "__main__":
    main()
