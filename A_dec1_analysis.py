#!/usr/bin/env python3
"""Analyze dec=1seg sweep (delta ∈ {1,2,4,8,16,32}, r=8.64, hl=4seg)."""
import os, re
import numpy as np

DELTAS = [1, 2, 4, 8, 16, 32]
R = 8.64
# Use sys.argv[1] to switch between sweep variants. Default: v2 (per-comp + Gud-Gu).
import sys
VARIANT = sys.argv[1] if len(sys.argv) > 1 else "dec1v2"
GSDEC_FMT = f"LOG_GREEDY_COST_BENEFIT_10_GS_us02_ewma_hl6291456_gsdec{{rt}}{VARIANT}d{{D}}_d{{D}}_pr{{rt}}.gsdec.log"
STAT_FMT  = f"LOG_GREEDY_COST_BENEFIT_10_GS_us02_ewma_hl6291456_gsdec{{rt}}{VARIANT}d{{D}}_d{{D}}.stat_pr{{rt}}"
BLK = 4096

def parse_gsdec(path):
    if not os.path.exists(path): return None
    rows = open(path).readlines()[1:]
    n=0; nR=0; nL=0; alt=0; same=0
    Gu=[]; Gud=[]; Fu=[]; Fud=[]; lhs=[]; rhs=[]; util=[]
    prev_dec=None; n_gud_neg=0; runs=[]; cur_run=0; cur_dec=None
    for line in rows:
        t = line.split()
        if len(t) < 11: continue
        n += 1
        Gu_=float(t[4]); Fu_=float(t[5]); Gud_=float(t[6]); Fud_=float(t[7])
        lhs_=float(t[8]); rhs_=float(t[9]); dec=t[10]; util_=float(t[11])
        Gu.append(Gu_); Fu.append(Fu_); Gud.append(Gud_); Fud.append(Fud_)
        lhs.append(lhs_); rhs.append(rhs_); util.append(util_)
        if dec=="RAISE": nR+=1
        elif dec=="LOWER": nL+=1
        if prev_dec is not None:
            if dec == prev_dec: same+=1
            else: alt+=1
        if Gud_ < 0: n_gud_neg += 1
        prev_dec = dec
        if dec == cur_dec: cur_run += 1
        else:
            if cur_dec is not None: runs.append((cur_run, cur_dec))
            cur_run = 1; cur_dec = dec
    if cur_dec is not None: runs.append((cur_run, cur_dec))
    return {
        "n": n, "nR": nR, "nL": nL, "alt": alt, "same": same,
        "n_gud_neg": n_gud_neg,
        "Gu_mean": np.mean(Gu), "Gud_mean": np.mean(Gud),
        "Fu_mean": np.mean(Fu), "Fud_mean": np.mean(Fud),
        "lhs_mean": np.mean(lhs), "rhs_mean": np.mean(rhs),
        "util_mean": np.mean(util), "util_max": np.max(util), "util_min": np.min(util),
        "max_run": max(r for r,_ in runs),
        "rhs_max": np.max(rhs), "lhs_max": np.max(lhs),
    }

KEYS = ("write_size_to_cache", "global_valid_blocks", "total_cache_size",
        "evicted_blocks", "compacted_blocks", "gc_victim_count")
def parse_stat_last(path):
    if not os.path.exists(path): return None
    last = None
    with open(path) as f:
        for line in f:
            if "LOG_GREEDY_COST_BENEFIT" in line and "write_size_to_cache" in line:
                last = line
    if last is None: return None
    d = {}
    for k in KEYS:
        m = re.search(rf"{k}:?\s+(\d+)", last)
        if m: d[k] = int(m.group(1))
    return d

def main(R):
    rt = str(R).replace(".", "")
    print(f"\n=== r={R} ===")
    print(f"{'D':>3} {'n':>5} {'RAISE%':>7} {'LOWER%':>7} {'alt%':>6} {'max_run':>8} {'G_ud<0':>7} | "
          f"{'util':>7} {'util max':>9} | {'LHS':>9} {'RHS':>9} | {'BUtil':>6} {'comp TB':>8} {'evict TB':>9} {'TEC':>9}")
    for D in DELTAS:
        st = parse_gsdec(GSDEC_FMT.format(rt=rt, D=D))
        if st is None:
            print(f"{D:>3}  [missing]"); continue
        sf = parse_stat_last(STAT_FMT.format(rt=rt, D=D))
        if sf is None:
            print(f"{D:>3}  stat missing"); continue
        host  = sf["write_size_to_cache"] / 1e12
        comp  = sf["compacted_blocks"] * BLK / 1e12
        evict = sf["evicted_blocks"]   * BLK / 1e12
        tec   = host + comp + R * evict
        butil = sf["global_valid_blocks"] / (sf["total_cache_size"] / BLK)
        N = st["n"]
        ap = 100*st["alt"]/(st["alt"]+st["same"]) if (st["alt"]+st["same"])>0 else 0
        print(f"{D:>3} {N:>5} {100*st['nR']/N:>6.1f}% {100*st['nL']/N:>6.1f}% "
              f"{ap:>5.1f}% {st['max_run']:>8} {st['n_gud_neg']:>7} | "
              f"{st['util_mean']:>7.3f} {st['util_max']:>9.3f} | "
              f"{st['lhs_mean']:>9.4f} {st['rhs_mean']:>9.4f} | "
              f"{butil:>6.3f} {comp:>8.3f} {evict:>9.3f} {tec:>9.2f}")

if __name__ == "__main__":
    main(8.64)
