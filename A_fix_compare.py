#!/usr/bin/env python3
"""Compare buggy vs fixed gsdec logs.

Buggy file: G_ud = compaction_ratio_in_ghost_cache.value() with non-monotone counter
            → can go negative, can be < G_u
Fixed file: G_ud = same .value() but counter is now monotone (only Σ dt*rate)
            → semantically becomes G_extra (= old G_ud - G_u), always ≥ 0
            → decision now: RHS = G_ud directly (no subtraction)

So in fixed files we expect: G_ud ≥ 0, NEVER G_ud < 0.
"""
import os
import numpy as np

SEGS = [1, 2, 4, 8, 16]
SETS = [
    ("buggy r=8.64", "LOG_GREEDY_COST_BENEFIT_10_GS_us02_ewma_hl1572864_gsdec864_segs{s}_pr864.gsdec.log"),
    ("fixed r=8.64", "LOG_GREEDY_COST_BENEFIT_10_GS_us02_ewma_hl1572864_gsdec864fix_segs{s}_pr864.gsdec.log"),
    ("fixed r=2.88", "LOG_GREEDY_COST_BENEFIT_10_GS_us02_ewma_hl1572864_gsdec288fix_segs{s}_pr288.gsdec.log"),
]

def summarize(path):
    if not os.path.exists(path): return None
    rows = open(path).readlines()[1:]
    n = 0; n_raise = 0; n_lower = 0; n_gud_neg = 0; n_lhs_neg = 0
    Gu=[]; Gud=[]; Fu=[]; Fud=[]
    for line in rows:
        t = line.split()
        if len(t) < 11: continue
        n += 1
        gu_ = float(t[4]); fu_ = float(t[5]); gud_ = float(t[6]); fud_ = float(t[7])
        dec = t[10]
        Gu.append(gu_); Fu.append(fu_); Gud.append(gud_); Fud.append(fud_)
        if dec == "RAISE": n_raise += 1
        elif dec == "LOWER": n_lower += 1
        if gud_ < 0: n_gud_neg += 1
        if fu_ < fud_: n_lhs_neg += 1
    return {
        "n": n, "n_raise": n_raise, "n_lower": n_lower,
        "n_gud_neg": n_gud_neg, "n_lhs_neg": n_lhs_neg,
        "Gu_mean": np.mean(Gu), "Gud_mean": np.mean(Gud),
        "Fu_mean": np.mean(Fu), "Fud_mean": np.mean(Fud),
        "Gud_min": np.min(Gud), "Gud_max": np.max(Gud),
    }

for label, fmt in SETS:
    print(f"\n=== {label} ===")
    hdr = f"{'seg':>3} {'n':>5} {'RAISE%':>7} {'LOWER%':>7} {'G_ud<0':>7} {'%':>5} {'Gu':>7} {'Gud':>7} {'Fu':>7} {'Fud':>7} {'Gud_min':>9} {'Gud_max':>9}"
    print(hdr); print("-"*len(hdr))
    for s in SEGS:
        st = summarize(fmt.format(s=s))
        if st is None:
            print(f"{s:>3}  [missing]"); continue
        n = st["n"]
        print(f"{s:>3} {n:>5} {100*st['n_raise']/n:>6.1f}% {100*st['n_lower']/n:>6.1f}% "
              f"{st['n_gud_neg']:>7} {100*st['n_gud_neg']/n:>4.1f}% "
              f"{st['Gu_mean']:>7.3f} {st['Gud_mean']:>7.3f} {st['Fu_mean']:>7.3f} {st['Fud_mean']:>7.3f} "
              f"{st['Gud_min']:>9.3f} {st['Gud_max']:>9.3f}")
