#!/usr/bin/env python3
"""Compare ghost-predicted inv_rate vs REAL-victim inv_rate.

Columns (1-based) in gsdec format (63 cols):
  35: gcsim_m            — # of segments ghost would pick to free 1 seg (m)
  48: raw_gud            — instantaneous Gud this period
  49: raw_compact        — realized compact this period
  60: gud_sum_inv_rate     (sum over ghost-picked victims, EWMA)
  61: gud_sum_inv_rate_lf  (sum over ghost-picked victims, raw 1-seg window)
  62: real_inv_rate        (mean per real-compacted victim during period, EWMA)
  63: real_inv_rate_lf     (mean per real-compacted victim during period, raw)

Forward-aligned: ghost predicts the next period [bdry_N, bdry_{N+1}].
Real victims processed during that interval are accumulated and their mean
inv_rate is reported on row N+1.
"""
import sys
from pathlib import Path

def parse(path):
    rows = []
    with open(path) as f:
        next(f)  # header
        for ln in f:
            parts = ln.split()
            if len(parts) < 63:
                continue
            try:
                rows.append({
                    "decision":              parts[10],
                    "raw_compact":           float(parts[48]),
                    "raw_gud":               float(parts[47]),
                    "gcsim_m":               float(parts[34]),
                    "gud_sum_inv_rate":      float(parts[59]),  # col 60
                    "gud_sum_inv_rate_lf":   float(parts[60]),  # col 61
                    "real_inv_rate":         float(parts[61]),  # col 62
                    "real_inv_rate_lf":      float(parts[62]),  # col 63
                })
            except (ValueError, IndexError):
                continue
    return rows


def build_pairs(rows):
    """Forward-aligned pairs: row N ghost vs row N+1 real."""
    pairs = []
    for i, r in enumerate(rows):
        if r["decision"] != "RAISE":
            continue
        if i + 1 >= len(rows):
            continue
        m = r["gcsim_m"]
        if m <= 0:
            continue
        ghost_ewma_mean = r["gud_sum_inv_rate"] / m
        ghost_raw_mean  = r["gud_sum_inv_rate_lf"] / m
        real_ewma = rows[i + 1]["real_inv_rate"]
        real_raw  = rows[i + 1]["real_inv_rate_lf"]
        if real_ewma <= 0 and real_raw <= 0:
            continue   # no real compaction in this period
        pairs.append({
            "ghost_ewma_mean": ghost_ewma_mean,
            "ghost_raw_mean":  ghost_raw_mean,
            "real_ewma":       real_ewma,
            "real_raw":        real_raw,
            "raw_gud":         r["raw_gud"],
            "raw_compact":     rows[i+1]["raw_compact"],
        })
    return pairs[100:]   # skip warmup


def stats(pairs, pkey, akey):
    n = len(pairs)
    if n == 0:
        return None
    sp = sum(p[pkey] for p in pairs)
    sa = sum(p[akey] for p in pairs)
    mp, ma = sp / n, sa / n
    over_pct = (sp / sa - 1.0) * 100 if sa > 0 else 0.0
    mae = sum(abs(p[pkey] - p[akey]) for p in pairs) / n
    bias = sum(p[pkey] - p[akey] for p in pairs) / n
    num = sum((p[pkey] - mp) * (p[akey] - ma) for p in pairs)
    den_p = (sum((p[pkey] - mp) ** 2 for p in pairs)) ** 0.5
    den_a = (sum((p[akey] - ma) ** 2 for p in pairs)) ** 0.5
    r = num / (den_p * den_a) if (den_p > 0 and den_a > 0) else 0.0
    return (n, mp, ma, over_pct, mae, bias, r)


def analyze_one(label, rows_v2, rows_v3):
    """Compare ghost vs real for v2 (partial) and v3 (seg_ago closed-1-seg)."""
    pairs_v2 = build_pairs(rows_v2)
    pairs_v3 = build_pairs(rows_v3)
    print(f"\n=== {label} ===")
    print(f"  pairs: v2={len(pairs_v2)}, v3={len(pairs_v3)}")

    # ─── EWMA comparison (col 60/m vs col 62) — same across v2/v3 ───
    # (real_inv_rate col 62 = EWMA mean per real victim — unchanged by v3 fix)
    s_ewma = stats(pairs_v3, "ghost_ewma_mean", "real_ewma")
    n, mp, ma, op, mae, b, r = s_ewma
    print(f"\n  [EWMA] ghost_mean vs real_mean (per-victim, EWMA α=0.1)")
    print(f"    n={n:5d}  mean_pred={mp:.6f}  mean_real={ma:.6f}  "
          f"over%={op:+.2f}  MAE={mae:.6f}  bias={b:+.6f}  r={r:.3f}")

    # ─── RAW comparison v2 (partial window for real) ───
    s_raw_v2 = stats(pairs_v2, "ghost_raw_mean", "real_raw")
    n, mp, ma, op, mae, b, r = s_raw_v2
    print(f"\n  [RAW v2 — real has PARTIAL window, ghost has 1-seg]")
    print(f"    n={n:5d}  mean_pred={mp:.6f}  mean_real={ma:.6f}  "
          f"over%={op:+.2f}  MAE={mae:.6f}  bias={b:+.6f}  r={r:.3f}")

    # ─── RAW comparison v3 (seg_ago closed-1-seg, apples-to-apples) ───
    s_raw_v3 = stats(pairs_v3, "ghost_raw_mean", "real_raw")
    n, mp, ma, op, mae, b, r = s_raw_v3
    print(f"\n  [RAW v3 — both 1-seg closed window, apples-to-apples]")
    print(f"    n={n:5d}  mean_pred={mp:.6f}  mean_real={ma:.6f}  "
          f"over%={op:+.2f}  MAE={mae:.6f}  bias={b:+.6f}  r={r:.3f}")

    # ─── v2 vs v3 of real_raw ONLY (how noisy was v2?) ───
    # Pair them by index (same trace, same row).
    real_v2 = [p["real_raw"] for p in pairs_v2]
    real_v3 = [p["real_raw"] for p in pairs_v3]
    n_cmp = min(len(real_v2), len(real_v3))
    if n_cmp > 0:
        mean_v2 = sum(real_v2[:n_cmp]) / n_cmp
        mean_v3 = sum(real_v3[:n_cmp]) / n_cmp
        diff_pct = (mean_v2 / mean_v3 - 1.0) * 100 if mean_v3 > 0 else 0.0
        # Correlate v2 vs v3
        mv2, mv3 = mean_v2, mean_v3
        num = sum((real_v2[i] - mv2) * (real_v3[i] - mv3) for i in range(n_cmp))
        dp = (sum((real_v2[i] - mv2)**2 for i in range(n_cmp))) ** 0.5
        da = (sum((real_v3[i] - mv3)**2 for i in range(n_cmp))) ** 0.5
        rr = num / (dp * da) if (dp > 0 and da > 0) else 0.0
        print(f"\n  [v2 vs v3 real_raw — same column, different window]")
        print(f"    mean_v2={mean_v2:.6f}  mean_v3={mean_v3:.6f}  "
              f"v2/v3-1={diff_pct:+.2f}%  pearson_r={rr:.3f}")


def main():
    traces = [
        ("dwpd01to1",
         "/home/sejun000/ssd_waf/GS_FINAL_realinv_dwpd01to1_pr864.gsdec.log",
         "/home/sejun000/ssd_waf/GS_FINAL_realinv_v3_dwpd01to1_pr864.gsdec.log"),
        ("dwpd1to2_4x",
         "/home/sejun000/ssd_waf/GS_FINAL_realinv_dwpd1to2_pr864.gsdec.log",
         "/home/sejun000/ssd_waf/GS_FINAL_realinv_v3_dwpd1to2_pr864.gsdec.log"),
        ("dwpd2_5x",
         "/home/sejun000/ssd_waf/GS_FINAL_realinv_dwpd2_5x_pr864.gsdec.log",
         "/home/sejun000/ssd_waf/GS_FINAL_realinv_v3_dwpd2_5x_pr864.gsdec.log"),
    ]
    for name, v2, v3 in traces:
        if not Path(v2).exists() or not Path(v3).exists():
            print(f"[skip] {name}")
            continue
        rows_v2 = parse(v2)
        rows_v3 = parse(v3)
        analyze_one(name, rows_v2, rows_v3)


if __name__ == "__main__":
    main()
