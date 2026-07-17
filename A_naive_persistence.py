#!/usr/bin/env python3
"""Compare naive 'past compact_avg' baseline vs Gud predictor.

Forward-aligned: RAISE row N predicts the period (N → N+1), realization is
the raw_compact of row N+1.

Predictors at row N:
  P1) raw_gud (Gud at N)                    — ghost prediction
  P2) compact_avg at N                       — EWMA of past compactions (auto-decay)
  P3) compact_avg at most recent prior RAISE — naive persistence on RAISE iters
  P4) raw_compact at most recent prior RAISE — pure previous-realization

Compare against actual = raw_compact at N+1.
"""
import sys
from pathlib import Path

# Column indices (1-based per gsdec header)
COL = {
    "ts": 1, "segs": 2, "r": 3, "waf": 4,
    "G_u": 5, "F_u": 6, "G_ud": 7, "F_ud": 8,
    "LHS": 9, "RHS": 10, "decision": 11,
    "cur_util": 12, "tgt_before": 13, "tgt_after": 14,
    "compact_avg": 23, "flush_avg": 24, "compact_evt": 25,
    "raw_gud": 48, "raw_compact": 49,
}

def analyze(path):
    rows = []
    with open(path) as f:
        next(f)  # skip header
        for ln in f:
            parts = ln.split()
            if len(parts) < 50:
                continue
            try:
                rows.append({
                    "decision": parts[10],
                    "compact_avg": float(parts[22]),
                    "raw_gud": float(parts[47]),
                    "raw_compact": float(parts[48]),
                })
            except (ValueError, IndexError):
                continue

    # For each RAISE row N, build predictor / realization pairs.
    # Realization = next row (N+1) raw_compact.
    pairs = []
    prev_raise_compact_avg = None
    prev_raise_raw_compact = None
    for i, r in enumerate(rows):
        if r["decision"] != "RAISE":
            continue
        if i + 1 >= len(rows):
            continue
        actual = rows[i + 1]["raw_compact"]
        pairs.append({
            "P1_gud":            r["raw_gud"],
            "P2_compact_avg_N":  r["compact_avg"],
            "P3_prev_raise_avg": prev_raise_compact_avg if prev_raise_compact_avg is not None else r["compact_avg"],
            "P4_prev_raise_raw": prev_raise_raw_compact if prev_raise_raw_compact is not None else 0.0,
            "actual":            actual,
        })
        prev_raise_compact_avg = r["compact_avg"]
        prev_raise_raw_compact = r["raw_compact"]

    # Skip warmup (first ~100 RAISEs)
    pairs = pairs[100:]
    if not pairs:
        return None

    def stats(predictor_key):
        n = len(pairs)
        sum_pred = sum(p[predictor_key] for p in pairs)
        sum_actual = sum(p["actual"] for p in pairs)
        # Absolute errors
        abs_err = [abs(p[predictor_key] - p["actual"]) for p in pairs]
        sq_err  = [(p[predictor_key] - p["actual"])**2 for p in pairs]
        # Bias
        signed = [p[predictor_key] - p["actual"] for p in pairs]
        # Over-pred ratio: sum_pred / sum_actual - 1
        over_pct = (sum_pred / sum_actual - 1.0) * 100 if sum_actual > 0 else 0.0
        mae = sum(abs_err) / n
        rmse = (sum(sq_err) / n) ** 0.5
        bias = sum(signed) / n
        # MAPE excluding zero actuals
        nonzero = [(p[predictor_key], p["actual"]) for p in pairs if p["actual"] > 0.01]
        if nonzero:
            mape = sum(abs(a - b) / b for a, b in nonzero) / len(nonzero) * 100
        else:
            mape = float("nan")
        return {
            "n": n,
            "mean_pred": sum_pred / n,
            "mean_actual": sum_actual / n,
            "over_pct": over_pct,
            "MAE": mae,
            "RMSE": rmse,
            "bias": bias,
            "MAPE%": mape,
        }

    out = {
        "P1_gud":             stats("P1_gud"),
        "P2_compact_avg_N":   stats("P2_compact_avg_N"),
        "P3_prev_raise_avg":  stats("P3_prev_raise_avg"),
        "P4_prev_raise_raw":  stats("P4_prev_raise_raw"),
    }
    return out


def main():
    traces = [
        ("dwpd01to1",  "/home/sejun000/ssd_waf/GS_FINAL_classinv_dwpd01to1_kvalraw_pr864.gsdec.log"),
        ("dwpd1to2_4x","/home/sejun000/ssd_waf/GS_FINAL_classinv_dwpd1to2_kvalraw_pr864.gsdec.log"),
        ("dwpd2_5x",   "/home/sejun000/ssd_waf/GS_FINAL_classinv_dwpd2_5x_kvalraw_pr864.gsdec.log"),
    ]
    for name, path in traces:
        if not Path(path).exists():
            print(f"[skip] {name}: {path} not found")
            continue
        print(f"\n=== {name} ===")
        res = analyze(path)
        if res is None:
            print("  (no RAISE pairs)")
            continue
        # Print as a small table
        keys = ["n", "mean_pred", "mean_actual", "over_pct", "MAE", "RMSE", "bias", "MAPE%"]
        hdr = f"{'predictor':<22}" + "".join(f"{k:>11}" for k in keys)
        print(hdr)
        print("-" * len(hdr))
        for label, s in res.items():
            line = f"{label:<22}"
            for k in keys:
                v = s[k]
                if k == "n":
                    line += f"{int(v):>11d}"
                else:
                    line += f"{v:>11.4f}"
            print(line)


if __name__ == "__main__":
    main()
