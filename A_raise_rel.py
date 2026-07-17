#!/usr/bin/env python3
"""RAISE rows: 상대 오차.
정의:
  t        : RAISE 결정한 row
  t+delta  : 그 다음 row (= row t+1, 시스템이 한 step 진행한 시점)
  G^{(t)}(u+delta) = G_ud[t]   (t 에서 예측한 u+delta 에서의 G)
  G(t+delta)       = G_u[t+1]  (t+delta 에서 실측한 G)
  조건 G(t+delta) > 0 만 집계, 오차율 = |G_ud[t] - G_u[t+1]| / G_u[t+1]
  F 도 동일.
"""
import os, numpy as np

SEGS = [1, 2, 4, 8, 16]
GSDEC_FMT = "LOG_GREEDY_COST_BENEFIT_10_GS_us02_ewma_hl1572864_gsdec864_segs{s}_pr864.gsdec.log"

def parse(path):
    rows = []
    with open(path) as f:
        next(f)
        for line in f:
            t = line.split()
            if len(t) < 18: continue
            rows.append((float(t[4]), float(t[5]), float(t[6]), float(t[7]), t[10]))
    return rows  # G_u, F_u, G_ud, F_ud, decision

def stats(a):
    if len(a) == 0: return [np.nan]*5
    a = np.asarray(a)
    return [a.mean(), np.median(a), np.percentile(a,75), np.percentile(a,90), np.percentile(a,99)]

print("RAISE rows: 상대 오차 = |pred[t] - actual[t+1]| / actual[t+1]")
print("(actual = G_u[t+1] 또는 F_u[t+1], > 0 인 row 만 집계)")
print()
hdr = f"{'seg':>3} {'n_R':>5} | {'nG':>4} {'rG mean':>8} {'rG med':>8} {'rG p75':>8} {'rG p90':>8} {'rG p99':>8} " \
      f"| {'nF':>4} {'rF mean':>8} {'rF med':>8} {'rF p75':>8} {'rF p90':>8} {'rF p99':>8}"
print(hdr); print("-"*len(hdr))
for s in SEGS:
    p = GSDEC_FMT.format(s=s)
    if not os.path.exists(p): continue
    rows = parse(p)
    rG, rF, nR = [], [], 0
    for i in range(len(rows) - 1):
        Gu0, Fu0, Gud, Fud, dec = rows[i]
        if dec != "RAISE": continue
        nR += 1
        Gu1 = rows[i+1][0]; Fu1 = rows[i+1][1]
        if Gu1 > 0: rG.append(abs(Gud - Gu1) / Gu1)
        if Fu1 > 0: rF.append(abs(Fud - Fu1) / Fu1)
    gG = stats(rG); gF = stats(rF)
    print(f"{s:>3} {nR:>5} | "
          f"{len(rG):>4} {gG[0]:>8.3f} {gG[1]:>8.3f} {gG[2]:>8.3f} {gG[3]:>8.3f} {gG[4]:>8.3f} | "
          f"{len(rF):>4} {gF[0]:>8.3f} {gF[1]:>8.3f} {gF[2]:>8.3f} {gF[3]:>8.3f} {gF[4]:>8.3f}")
