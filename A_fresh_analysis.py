#!/usr/bin/env python3
"""Full analysis of real-victim age/u distribution + fresh-victim hypothesis test.

Compare ghost-predicted u (= Gud / m, mean u of ghost-picked victims)
against real measured u (col 65, mean u of real-compacted victims).

If ghost u > real u systematically → ghost picks higher-u victims (lower invalidation)
→ cum_valid over-predicted → Gud over-bias. This is the score-evolution mechanism.

Also: count rows where any fresh (age<1seg) victim appears.
"""
from pathlib import Path

COL = {  # 1-based
    "decision": 11, "compact_avg": 23, "compact_evt": 25, "gcsim_m": 35,
    "raw_gud": 48, "raw_compact": 49,
    "gud_sum_inv_rate": 60, "gud_sum_inv_rate_lf": 61,
    "real_inv_rate": 62, "real_inv_rate_lf": 63,
    "real_age_segs": 64, "real_u": 65, "real_fresh_frac": 66, "real_fresh_u": 67,
}

def parse(path):
    rows = []
    with open(path) as f:
        next(f)
        for ln in f:
            p = ln.split()
            if len(p) < 67: continue
            try:
                rows.append({k: (p[v-1] if k == "decision" else float(p[v-1]))
                            for k, v in COL.items()})
            except: continue
    return rows


def analyze(label, path):
    rows = parse(path)

    # All RAISE rows with real compaction (age > 0 means dcomp_real > 0)
    raise_w_compact = [r for r in rows
                       if r["decision"] == "RAISE" and r["real_age_segs"] > 0]
    if not raise_w_compact:
        print(f"\n=== {label} === no RAISE+real-compact rows"); return

    n = len(raise_w_compact)
    # Skip first 100 warmup rows
    pairs = raise_w_compact[100:]
    n_p = len(pairs)

    print(f"\n=== {label}  (total RAISE w/ compact = {n}, after warmup = {n_p}) ===")

    # Distribution of real-victim age/u/fresh
    age_mean   = sum(r["real_age_segs"] for r in pairs) / n_p
    u_mean     = sum(r["real_u"] for r in pairs) / n_p
    fresh_mean = sum(r["real_fresh_frac"] for r in pairs) / n_p
    rows_with_fresh = sum(1 for r in pairs if r["real_fresh_frac"] > 0)
    fresh_pct_rows = rows_with_fresh / n_p * 100

    print(f"  age  mean={age_mean:8.2f} segs   (≈ {age_mean*1.572/1024:.2f} GB host writes)")
    print(f"  u    mean={u_mean:.4f}")
    print(f"  fresh_frac mean={fresh_mean:.4f}  ({fresh_mean*100:.2f}%)")
    print(f"  rows containing fresh victim: {rows_with_fresh}/{n_p}  ({fresh_pct_rows:.2f}%)")

    # Ghost u (= Gud / m) vs real u (col 65)
    # Caveat: this uses raw_gud (period instantaneous) divided by m.
    gu_sum = 0.0; ru_sum = 0.0; both_n = 0
    for r in pairs:
        if r["gcsim_m"] <= 0: continue
        ghost_u = r["raw_gud"] / r["gcsim_m"]
        gu_sum += ghost_u
        ru_sum += r["real_u"]
        both_n += 1
    if both_n > 0:
        ghost_u_avg = gu_sum / both_n
        real_u_avg  = ru_sum / both_n
        print(f"\n  Ghost picks mean u (= Gud/m) = {ghost_u_avg:.4f}")
        print(f"  Real  picks mean u (col 65)  = {real_u_avg:.4f}")
        print(f"  ghost_u / real_u = {ghost_u_avg/real_u_avg:.4f}")

    # Cross-trace: also report Gud vs compact_avg mean
    Gud_mean   = sum(r["raw_gud"] for r in pairs) / n_p
    cmpct_mean = sum(r["compact_avg"] for r in pairs) / n_p
    raw_compact_mean = sum(r["raw_compact"] for r in pairs) / n_p
    print(f"\n  Gud mean         = {Gud_mean:.4f}")
    print(f"  compact_avg mean = {cmpct_mean:.4f}")
    print(f"  raw_compact mean = {raw_compact_mean:.4f}")
    print(f"  Gud / raw_compact = {Gud_mean/raw_compact_mean:.4f}   (over-pred ratio)")

    # Age histogram (rough buckets)
    buckets = [0, 1, 5, 10, 30, 100, 300, 1000, 10000]
    counts = [0] * (len(buckets)-1)
    for r in pairs:
        a = r["real_age_segs"]
        for i in range(len(buckets)-1):
            if buckets[i] <= a < buckets[i+1]:
                counts[i] += 1; break
    print(f"\n  Age distribution (segments):")
    for i in range(len(buckets)-1):
        pct = counts[i] / n_p * 100
        bar = "█" * int(pct / 2)
        print(f"    [{buckets[i]:>5} ~ {buckets[i+1]:>5}) : {counts[i]:>5} ({pct:5.2f}%) {bar}")


def main():
    for name, path in [
        ("dwpd01to1",   "/home/sejun000/ssd_waf/GS_FINAL_fresh_dwpd01to1_pr864.gsdec.log"),
        ("dwpd1to2_4x", "/home/sejun000/ssd_waf/GS_FINAL_fresh_dwpd1to2_pr864.gsdec.log"),
        ("dwpd2_5x",    "/home/sejun000/ssd_waf/GS_FINAL_fresh_dwpd2_5x_pr864.gsdec.log"),
    ]:
        if Path(path).exists():
            analyze(name, path)


if __name__ == "__main__":
    main()
