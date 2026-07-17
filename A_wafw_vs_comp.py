#!/usr/bin/env python3
# hi REFLASH: does compaction(raise) intensify as waf_w rises?
#   waf_w  = policy input (720GiB tumbling cold WAF, clamp>=1)   [log_cache.cpp:263-267,812]
#   comp_rate = d(compacted_blocks*4096)/d(host_writes)  over 1TiB sliding window
#               = cache-side compaction writes per host write (raise intensity proxy)
import bisect
STAT = "dwpdhi_512_inj10_r864.stat"
TIB = 2**40; BLK = 4096
BUCKET = 188743680 * 4096           # 720 GiB tumbling (waf_w window)
WIN = 1.0 * TIB                     # comp_rate sliding window

def col(tk, k):
    for i, t in enumerate(tk):
        if t == k:
            return float(tk[i+1])
    return None

xs, cb, fh, fn = [], [], [], []
with open(STAT) as f:
    for line in f:
        tk = line.split()
        w = col(tk, "write_size_to_cache:"); c = col(tk, "compacted_blocks:")
        h = col(tk, "ftl_host_pages:"); n = col(tk, "ftl_nand_pages:")
        if None in (w, c, h, n):
            continue
        xs.append(w / TIB); cb.append(c * BLK); fh.append(h * BLK); fn.append(n * BLK)

# waf_w (tumbling 720GiB)
wafw = []; last_fh, last_fn, cur, nb = fh[0], fn[0], 0.0, BUCKET
for i in range(len(xs)):
    if xs[i] * TIB >= nb:
        dh, dn = fh[i] - last_fh, fn[i] - last_fn
        cur = (dn / dh) if dh > 0 else 0.0
        last_fh, last_fn = fh[i], fn[i]; nb += BUCKET
    wafw.append(cur if cur > 0 else 1.0)

# comp_rate (1TiB sliding): compaction bytes per host byte
comp_rate = [float("nan")] * len(xs); j = 0
for i in range(len(xs)):
    while xs[i] - xs[j] > WIN / TIB:
        j += 1
    dx = (xs[i] - xs[j]) * TIB
    if dx > 0:
        comp_rate[i] = (cb[i] - cb[j]) / dx

print("  host(TiB)  waf_w   comp_rate(comp_B/host_B)")
for q in (10, 18, 22, 24, 25, 26, 28, 30, 32, 35, 40, 45, 50):
    k = bisect.bisect_left(xs, q)
    if k < len(xs) and comp_rate[k] == comp_rate[k]:
        print("   %5.0f    %.3f    %.3f" % (q, wafw[k], comp_rate[k]))

# correlation post-onset (waf_w>1 region, x>=25)
import math
pts = [(wafw[i], comp_rate[i]) for i in range(len(xs))
       if xs[i] >= 25 and comp_rate[i] == comp_rate[i] and wafw[i] > 0]
n = len(pts)
mx = sum(p[0] for p in pts) / n; my = sum(p[1] for p in pts) / n
sxy = sum((p[0]-mx)*(p[1]-my) for p in pts)
sxx = sum((p[0]-mx)**2 for p in pts); syy = sum((p[1]-my)**2 for p in pts)
r = sxy / math.sqrt(sxx*syy) if sxx > 0 and syy > 0 else float("nan")
print("\n  post-onset(>=25TiB) Pearson r(waf_w, comp_rate) = %.3f  (n=%d windows)" % (r, n))
print("  pre-onset comp_rate avg (10-23T)  = %.3f" %
      (sum(comp_rate[i] for i in range(len(xs)) if 10 <= xs[i] < 23 and comp_rate[i]==comp_rate[i]) /
       max(1, sum(1 for i in range(len(xs)) if 10 <= xs[i] < 23 and comp_rate[i]==comp_rate[i]))))
print("  post-onset comp_rate avg (25-52T) = %.3f" %
      (sum(comp_rate[i] for i in range(len(xs)) if 25 <= xs[i] < 52 and comp_rate[i]==comp_rate[i]) /
       max(1, sum(1 for i in range(len(xs)) if 25 <= xs[i] < 52 and comp_rate[i]==comp_rate[i]))))
