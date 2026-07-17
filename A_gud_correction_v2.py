#!/usr/bin/env python3
"""Subtractive vs multiplicative Gud correction.

Gud and inv_rate share denominator (host pages) → subtraction is dimensionally
clean: Gud - Σ inv_rate = "net new valid copied after subtracting in-period
invalidations on the picked victims".

Variants:
  S1: Gud - ghost_inv_sum_raw         (predictive, no future info)
  S2: Gud - real_inv_sum_raw          (oracle, future info)
  M1: Gud × (1 - ghost_inv_sum_raw)   (multiplicative ghost-side)
  M2: Gud × (1 - real_inv_sum_raw)    (oracle)
  K:  Gud × (ghost_inv_avg / real_inv_avg)    — trace-level constant ratio
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
                    "compact_avg":         float(p[22]),
                    "compact_evt":         int(float(p[24])),
                    "gcsim_m":             float(p[34]),
                    "raw_gud":             float(p[47]),
                    "raw_compact":         float(p[48]),
                    "gud_sum_inv_rate_lf": float(p[60]),
                    "real_inv_rate_lf":    float(p[62]),
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
        ghost_inv_sum = r["gud_sum_inv_rate_lf"]            # Σ over m picks
        real_inv_sum  = rows[i+1]["real_inv_rate_lf"] * K   # mean × K = sum
        pairs.append({
            "Gud":           r["raw_gud"],
            "compact_avg":   r["compact_avg"],
            "actual":        rows[i+1]["raw_compact"],
            "ghost_inv_sum": ghost_inv_sum,
            "real_inv_sum":  real_inv_sum,
        })
    pairs = pairs[100:]
    if not pairs: return

    # Trace-level constant ratio (sum of ghost / sum of real)
    sg = sum(p["ghost_inv_sum"] for p in pairs)
    sr = sum(p["real_inv_sum"]  for p in pairs)
    K_factor = sg / sr if sr > 0 else 1.0

    print(f"\n=== {label}  ({len(pairs)} pairs) ===")
    print(f"  ghost_inv_sum mean = {sg/len(pairs):.4f}")
    print(f"  real_inv_sum  mean = {sr/len(pairs):.4f}")
    print(f"  Gud           mean = {sum(p['Gud'] for p in pairs)/len(pairs):.4f}")
    print(f"  K_factor (ghost/real) = {K_factor:.4f}")

    for p in pairs:
        gi = p["ghost_inv_sum"]; ri = p["real_inv_sum"]
        p["S1_Gud_minus_ghost"] = max(0.0, p["Gud"] - gi)
        p["S2_Gud_minus_real"]  = max(0.0, p["Gud"] - ri)
        p["M1_Gud_x_1minus_ghost"] = p["Gud"] * max(0.0, 1.0 - gi)
        p["M2_Gud_x_1minus_real"]  = p["Gud"] * max(0.0, 1.0 - ri)
        p["K_Gud_x_K"] = p["Gud"] * K_factor

    def stats(pkey):
        n = len(pairs)
        sp = sum(p[pkey] for p in pairs)
        sa = sum(p["actual"] for p in pairs)
        mp, ma = sp/n, sa/n
        over = (sp/sa - 1.0)*100 if sa > 0 else 0.0
        mae = sum(abs(p[pkey]-p["actual"]) for p in pairs)/n
        bias = sum(p[pkey]-p["actual"] for p in pairs)/n
        num = sum((p[pkey]-mp)*(p["actual"]-ma) for p in pairs)
        dp = (sum((p[pkey]-mp)**2 for p in pairs))**0.5
        da = (sum((p["actual"]-ma)**2 for p in pairs))**0.5
        rr = num/(dp*da) if (dp>0 and da>0) else 0.0
        return (n, mp, ma, over, mae, bias, rr)

    print(f"\n  {'predictor':<32}{'mean_p':>10}{'mean_a':>10}{'over%':>9}{'MAE':>10}{'bias':>10}{'r':>8}")
    print("  " + "-"*89)
    for name, key in [
        ("Gud (baseline)",                "Gud"),
        ("compact_avg (naive)",           "compact_avg"),
        ("S1 Gud − ghost_inv_sum",        "S1_Gud_minus_ghost"),
        ("S2 Gud − real_inv_sum  (oracle)","S2_Gud_minus_real"),
        ("M1 Gud × (1−ghost_inv)",        "M1_Gud_x_1minus_ghost"),
        ("M2 Gud × (1−real_inv)  (oracle)","M2_Gud_x_1minus_real"),
        ("K  Gud × K_factor",             "K_Gud_x_K"),
    ]:
        n, mp, ma, over, mae, bias, rr = stats(key)
        print(f"  {name:<32}{mp:>10.4f}{ma:>10.4f}{over:>+9.2f}{mae:>10.4f}{bias:>+10.4f}{rr:>8.3f}")


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
