#!/usr/bin/env python3
"""Full victim-ID set comparison analysis.

Columns (0-based from p[]):
  10 decision
  22 compact_avg
  24 compact_evt (cum)
  34 gcsim_m
  47 raw_gud
  48 raw_compact
  59 gud_sum_inv_rate    (sum, EWMA)
  60 gud_sum_inv_rate_lf (sum, raw)
  61 real_inv_rate       (mean, EWMA)
  62 real_inv_rate_lf    (mean, raw)
  63 real_age_segs       (mean age of real victims, segs)
  64 real_u_mean         (mean u of real victims)
  65 real_fresh_frac
  66 real_fresh_u_mean
  67-70 g_vid[0..3]      (ghost picks IDs = create_timestamp)
  71-74 r_vid[0..3]      (real picks IDs)
"""
from pathlib import Path
from collections import Counter

def parse(path):
    rows = []
    with open(path) as f:
        next(f)
        for ln in f:
            p = ln.split()
            if len(p) < 75: continue
            try:
                rows.append({
                    "dec": p[10],
                    "compact_avg": float(p[22]),
                    "compact_evt": int(float(p[24])),
                    "gcsim_m":     float(p[34]),
                    "raw_gud":     float(p[47]),
                    "raw_compact": float(p[48]),
                    "real_age":    float(p[63]),
                    "real_u":      float(p[64]),
                    "g": [int(p[67]), int(p[68]), int(p[69]), int(p[70])],
                    "r": [int(p[71]), int(p[72]), int(p[73]), int(p[74])],
                })
            except: continue
    return rows


def analyze(label, path):
    rows = parse(path)
    # Forward-aligned: ghost picks at row N (RAISE) vs real picks at row N+1
    pairs = []
    for i in range(len(rows)-1):
        if rows[i]["dec"] != "RAISE": continue
        if rows[i+1]["real_age"] <= 0: continue  # no real compaction
        g_now = rows[i]["g"]
        r_next = rows[i+1]["r"]
        pairs.append({
            "g": g_now,
            "r": r_next,
            "ghost_m":  rows[i]["gcsim_m"],
            "raw_gud":  rows[i]["raw_gud"],
            "raw_compact": rows[i+1]["raw_compact"],
            "compact_avg": rows[i]["compact_avg"],
            "real_u":   rows[i+1]["real_u"],
            "real_age": rows[i+1]["real_age"],
            "K": rows[i+1]["compact_evt"] - rows[i]["compact_evt"],
        })
    pairs = pairs[100:]
    n = len(pairs)
    if n == 0:
        print(f"\n=== {label} === no pairs"); return
    print(f"\n=== {label}  (n={n} pairs forward-aligned RAISE→real) ===")

    # Counts
    g_cnt = lambda v: sum(1 for x in v if x != -1)
    r_cnt = lambda v: sum(1 for x in v if x != -1)
    gc_avg = sum(g_cnt(p["g"]) for p in pairs) / n
    rc_avg = sum(r_cnt(p["r"]) for p in pairs) / n
    print(f"  picks count avg: ghost raw = {gc_avg:.2f},  real K = {rc_avg:.2f}")
    print(f"  gcsim_m (col 35) avg = {sum(p['ghost_m'] for p in pairs)/n:.3f}  (fractional)")

    # Overlap stats
    top1_match = 0
    overlap_sum = 0
    overlap_dist = Counter()
    for p in pairs:
        gs = set(x for x in p["g"] if x != -1)
        rs = set(x for x in p["r"] if x != -1)
        ov = len(gs & rs)
        overlap_sum += ov
        overlap_dist[ov] += 1
        if p["g"][0] != -1 and p["g"][0] == p["r"][0]:
            top1_match += 1
    print(f"  top-1 match: {top1_match}/{n} = {top1_match/n*100:.2f}%")
    print(f"  mean |g ∩ r| = {overlap_sum/n:.3f}")
    print(f"  overlap distribution: {dict(sorted(overlap_dist.items()))}")
    no_overlap = overlap_dist.get(0, 0)
    print(f"  rows with zero overlap (no common victim): {no_overlap}/{n} = {no_overlap/n*100:.2f}%")

    # Real picks that ARE in ghost set (= "predicted correctly")
    # Real picks that are NOT in ghost set (= "missed by ghost")
    real_in_ghost = 0; real_not_in_ghost = 0
    for p in pairs:
        gs = set(x for x in p["g"] if x != -1)
        for x in p["r"]:
            if x == -1: continue
            if x in gs: real_in_ghost += 1
            else:       real_not_in_ghost += 1
    total_r = real_in_ghost + real_not_in_ghost
    if total_r > 0:
        print(f"  real picks IN ghost set:  {real_in_ghost}/{total_r} = {real_in_ghost/total_r*100:.2f}%")
        print(f"  real picks NOT in ghost:  {real_not_in_ghost}/{total_r} = {real_not_in_ghost/total_r*100:.2f}%")

    # u_ghost (= raw_gud / gcsim_m) vs u_real
    gu_sum = 0.0; ru_sum = 0.0; cnt = 0
    for p in pairs:
        if p["ghost_m"] <= 0: continue
        gu_sum += p["raw_gud"] / p["ghost_m"]
        ru_sum += p["real_u"]
        cnt += 1
    if cnt > 0:
        gu_avg = gu_sum / cnt
        ru_avg = ru_sum / cnt
        print(f"  ghost u (raw_gud/m): {gu_avg:.4f}")
        print(f"  real  u (col 65):    {ru_avg:.4f}")
        print(f"  u gap = (ghost - real)/real = {(gu_avg-ru_avg)/ru_avg*100:+.2f}%")
        # delay estimate
        u_drop = gu_avg - ru_avg
        # use trace-wide mean inv_rate (col 62/m)
        # rough: real_inv_rate field is mean per victim, ~0.05
        # delay = u_drop × seg_blocks / inv_rate
        # assume seg_blocks = 1572864
        seg_blocks = 1572864
        # need an inv_rate estimate — use raw mean
        # Actually we don't have it here, use a rough fixed value below
        # Approximate delay (in segments) = u_drop / inv_rate
        # inv_rate ~ 0.04 (mean across traces)
        for ir_assumed in [0.03, 0.05]:
            delay_segs = u_drop / ir_assumed
            print(f"  → if inv_rate={ir_assumed}, implied delay = {delay_segs:.2f} segments")

    # raw_gud vs raw_compact (Gud over-pred)
    sg = sum(p["raw_gud"] for p in pairs)
    sa = sum(p["raw_compact"] for p in pairs)
    print(f"  Gud over raw_compact: ({sg/n:.4f} / {sa/n:.4f} - 1) × 100 = {(sg/sa-1)*100:+.2f}%")


def main():
    for name, path in [
        ("dwpd01to1",   "/home/sejun000/ssd_waf/GS_FINAL_vidlog_dwpd01to1_pr864.gsdec.log"),
        ("dwpd1to2_4x", "/home/sejun000/ssd_waf/GS_FINAL_vidlog_dwpd1to2_pr864.gsdec.log"),
        ("dwpd2_5x",    "/home/sejun000/ssd_waf/GS_FINAL_vidlog_dwpd2_5x_pr864.gsdec.log"),
    ]:
        if Path(path).exists():
            analyze(name, path)


if __name__ == "__main__":
    main()
