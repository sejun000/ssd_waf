#!/usr/bin/env python3
"""Vary the delay multiplier in the subtraction model.

Gud_corrected = Gud - N × real_inv_sum

where N represents "how many segments worth of host writes elapse between
ghost prediction and real execution". N=1 is naive (1-seg delay), N>1 means
real victims age more before being picked.

Try N = 1, 2, 3, 5, 10 — find which N matches compact_avg's near-zero bias.
"""
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
                    "compact_avg":         float(p[22]),
                    "compact_evt":         int(float(p[24])),
                    "gcsim_m":             float(p[34]),
                    "raw_gud":             float(p[47]),
                    "raw_compact":         float(p[48]),
                    "gud_sum_inv_rate_lf": float(p[60]),
                    "real_inv_rate_lf":    float(p[62]),
                })
            except: continue
    return rows


def build_pairs(rows):
    pairs = []
    for i, r in enumerate(rows):
        if r["decision"] != "RAISE": continue
        if i+1 >= len(rows): continue
        m = r["gcsim_m"]
        if m <= 0: continue
        K = rows[i+1]["compact_evt"] - r["compact_evt"]
        if K <= 0: continue
        if rows[i+1]["real_inv_rate_lf"] <= 0: continue
        pairs.append({
            "Gud":          r["raw_gud"],
            "compact_avg":  r["compact_avg"],
            "actual":       rows[i+1]["raw_compact"],
            "real_inv_sum": rows[i+1]["real_inv_rate_lf"] * K,
        })
    return pairs[100:]


def analyze(label, path):
    rows = parse(path)
    pairs = build_pairs(rows)
    if not pairs: return
    sa = sum(p["actual"] for p in pairs)
    n = len(pairs)

    print(f"\n=== {label}  ({n} pairs) ===")
    print(f"  Gud mean={sum(p['Gud'] for p in pairs)/n:.4f}, "
          f"actual mean={sa/n:.4f}, "
          f"compact_avg mean={sum(p['compact_avg'] for p in pairs)/n:.4f}")

    print(f"\n  {'predictor':<28}{'mean_p':>10}{'over%':>9}{'MAE':>10}{'bias':>10}")
    print("  " + "-" * 67)
    # Baseline & naive
    for name, key in [("Gud (raw)", "Gud"), ("compact_avg", "compact_avg")]:
        sp = sum(p[key] for p in pairs)
        mae = sum(abs(p[key]-p["actual"]) for p in pairs)/n
        bias = sum(p[key]-p["actual"] for p in pairs)/n
        over = (sp/sa - 1.0)*100
        print(f"  {name:<28}{sp/n:>10.4f}{over:>+9.2f}{mae:>10.4f}{bias:>+10.4f}")
    # Delay-multiplier sweep
    for N in [1, 2, 3, 5, 7, 10]:
        sp = 0.0; mae = 0.0; bias = 0.0
        for p in pairs:
            pred = max(0.0, p["Gud"] - N * p["real_inv_sum"])
            sp   += pred
            mae  += abs(pred - p["actual"])
            bias += pred - p["actual"]
        over = (sp/sa - 1.0)*100
        print(f"  Gud − {N}×real_inv_sum{'':<10}{sp/n:>10.4f}{over:>+9.2f}{mae/n:>10.4f}{bias/n:>+10.4f}")


def main():
    for name, path in [
        ("dwpd01to1",   "/home/sejun000/ssd_waf/GS_FINAL_realinv_v3_dwpd01to1_pr864.gsdec.log"),
        ("dwpd1to2_4x", "/home/sejun000/ssd_waf/GS_FINAL_realinv_v3_dwpd1to2_pr864.gsdec.log"),
        ("dwpd2_5x",    "/home/sejun000/ssd_waf/GS_FINAL_realinv_v3_dwpd2_5x_pr864.gsdec.log"),
    ]:
        if Path(path).exists():
            analyze(name, path)


if __name__ == "__main__":
    main()
