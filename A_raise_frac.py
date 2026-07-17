#!/usr/bin/env python3
# hi REFLASH: raise(GC) fraction vs waf_w, binned in 96 GiB host-write windows.
#   decision recovered from stat: raise <=> target_valid_rate > U_B
#     (SUM_Final: target = U_B + util_step if raise, U_B - util_step if lower; log_cache.cpp:862-871)
#   U_B = global_valid_blocks*4096/total_cache_size
#   waf_w = 720GiB-tumbling cold WAF (clamp>=1), the policy input
import bisect, math
STAT = "dwpdhi_512_inj10_r864.stat"
TIB = 2**40; BLK = 4096
BUCKET = 188743680 * 4096          # waf_w tumbling window (~720 GiB)
BIN = 96 * 2**30                   # 96 GiB decision-fraction window

def col(tk, k):
    for i, t in enumerate(tk):
        if t == k:
            return float(tk[i+1])
    return None

W, U, TVR, FH, FN, US = [], [], [], [], [], []
with open(STAT) as f:
    for line in f:
        tk = line.split()
        w = col(tk, "write_size_to_cache:"); g = col(tk, "global_valid_blocks:")
        tc = col(tk, "total_cache_size:");   tv = col(tk, "target_valid_rate:")
        h = col(tk, "ftl_host_pages:");      n = col(tk, "ftl_nand_pages:")
        us = col(tk, "util_step:")
        if None in (w, g, tc, tv, h, n) or tc <= 0:
            continue
        W.append(w); U.append(g*BLK/tc); TVR.append(tv); FH.append(h*BLK); FN.append(n*BLK); US.append(us or 0)

# waf_w per row (720GiB tumbling)
wafw = []; lh, ln, cur, nb = FH[0], FN[0], 0.0, BUCKET
for i in range(len(W)):
    if W[i] >= nb:
        dh, dn = FH[i]-lh, FN[i]-ln
        cur = (dn/dh) if dh > 0 else 0.0
        lh, ln = FH[i], FN[i]; nb += BUCKET
    wafw.append(cur if cur > 0 else 1.0)

# raise indicator per row: target > U_B  (with small deadband = tiny)
raise_ind = [1 if (TVR[i] - U[i]) > 0 else 0 for i in range(len(W))]

# bin into 96 GiB windows
bins = {}
for i in range(len(W)):
    b = int(W[i] // BIN)
    bins.setdefault(b, []).append(i)

rows = []
for b in sorted(bins):
    idx = bins[b]
    hw = (b + 0.5) * BIN / TIB
    rf = sum(raise_ind[i] for i in idx) / len(idx)
    ww = sum(wafw[i] for i in idx) / len(idx)
    rows.append((hw, ww, rf, len(idx)))

print("  host(TiB)  waf_w  raise_frac  (n rows)")
for hw, ww, rf, n in rows:
    if hw < 6:    # skip warmup print, show sparse
        continue
    if abs(hw*1000 % 2000) < 100 or hw > 48:  # ~every 2 TiB
        print("   %5.1f    %.3f    %.3f     (%d)" % (hw, ww, rf, n))

# correlation post-onset (host>=25)
pts = [(ww, rf) for hw, ww, rf, n in rows if hw >= 25]
m = len(pts); mx = sum(p[0] for p in pts)/m; my = sum(p[1] for p in pts)/m
sxy = sum((p[0]-mx)*(p[1]-my) for p in pts)
sxx = sum((p[0]-mx)**2 for p in pts); syy = sum((p[1]-my)**2 for p in pts)
r = sxy/math.sqrt(sxx*syy) if sxx>0 and syy>0 else float("nan")
pre = [rf for hw, ww, rf, n in rows if 8 <= hw < 23]
post = [rf for hw, ww, rf, n in rows if 25 <= hw]
print("\n  raise_frac  pre-onset(8-23T) avg = %.3f" % (sum(pre)/len(pre)))
print("  raise_frac post-onset(>=25T) avg = %.3f" % (sum(post)/len(post)))
print("  Pearson r(waf_w, raise_frac) post-onset = %.3f  (n=%d bins)" % (r, m))

# save arrays for plotting
import json
json.dump({"hw":[x[0] for x in rows], "wafw":[x[1] for x in rows], "rf":[x[2] for x in rows]},
          open("A_raise_frac.json","w"))
print("  -> A_raise_frac.json")
