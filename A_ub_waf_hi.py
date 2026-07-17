#!/usr/bin/env python3
# hi REFLASH: U_B (cache buffer utilization) vs host writes, with cold WAF on right axis.
#   U_B = global_valid_blocks * 4096 / total_cache_size   (dp_optimizer.cpp:5 정의)
#   cold WAF = ftl_nand_pages / ftl_host_pages  (cumulative + 1TiB-windowed)
import sys
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

STAT = "dwpdhi_512_inj10_r864.stat"
TIB  = 2**40
BLK  = 4096
WIN  = 1.0 * TIB   # windowed-WAF host-write window

def col(toks, key):
    for i, t in enumerate(toks):
        if t == key:
            return float(toks[i+1])
    return None

xs, ub, tvr = [], [], []
fh, fn = [], []   # ftl_host_pages, ftl_nand_pages (cumulative)
with open(STAT) as f:
    for line in f:
        tk = line.split()
        w = col(tk, "write_size_to_cache:")
        g = col(tk, "global_valid_blocks:")
        tc = col(tk, "total_cache_size:")
        h = col(tk, "ftl_host_pages:")
        n = col(tk, "ftl_nand_pages:")
        t = col(tk, "target_valid_rate:")
        if None in (w, g, tc, h, n) or tc <= 0:
            continue
        xs.append(w / TIB)
        ub.append(g * BLK / tc)
        tvr.append(t if t is not None else float("nan"))
        fh.append(h * BLK)   # bytes
        fn.append(n * BLK)

# cumulative cold WAF
waf_cum = [(fn[i] / fh[i]) if fh[i] > 0 else float("nan") for i in range(len(xs))]

# windowed (1 TiB host-write) cold WAF
waf_win = [float("nan")] * len(xs)
j = 0
for i in range(len(xs)):
    while xs[i] - xs[j] > WIN / TIB:
        j += 1
    dh = fh[i] - fh[j]
    dn = fn[i] - fn[j]
    if dh > 0:
        waf_win[i] = dn / dh

# waf_w EXACTLY as the policy computes it: tumbling ~720 GiB host-write window
#   (periodic() per page, t>=188743680 -> current_waf = dcold_nand/dcold_host),
#   clamped: current_waf==0 -> 1.0   (log_cache.cpp:263-267, 812)
BUCKET = 188743680 * 4096   # host-write bytes per tumbling window
wafw = []
last_fh, last_fn, cur, nb = fh[0], fn[0], 0.0, BUCKET
for i in range(len(xs)):
    w = xs[i] * TIB
    if w >= nb:
        dh, dn = fh[i] - last_fh, fn[i] - last_fn
        cur = (dn / dh) if dh > 0 else 0.0
        last_fh, last_fn = fh[i], fn[i]
        nb += BUCKET
    wafw.append(cur if cur > 0 else 1.0)

# print waf_w around the WAF-onset point
import bisect
print("host_writes(TiB) -> waf_w(policy input):")
for q in (20, 22, 23, 24, 25, 26, 28, 30, 35, 40, 45, 50):
    k = bisect.bisect_left(xs, q)
    if k < len(xs):
        print("  %5.0fT  waf_w=%.3f  (U_B=%.3f, WAFwin=%.3f)" % (q, wafw[k], ub[k], waf_win[k]))

fig, axL = plt.subplots(figsize=(9, 5))
axL.plot(xs, ub, color="#1f77b4", lw=1.6, label="U_B (cache valid-rate)")
axL.plot(xs, tvr, color="#1f77b4", lw=1.0, ls=":", alpha=0.6, label="target_valid_rate")
axL.set_xlabel("host writes (TiB)")
axL.set_ylabel("U_B = global_valid_blocks·4K / cache_size", color="#1f77b4")
axL.tick_params(axis="y", labelcolor="#1f77b4")
axL.set_ylim(0, 1.0)
axL.grid(True, alpha=0.25)

axR = axL.twinx()
axR.plot(xs, wafw, color="#2ca02c", lw=1.8, label="waf_w (policy input, ~720GiB tumbling, clamp≥1)")
axR.plot(xs, waf_win, color="#d62728", lw=1.1, alpha=0.8, label="cold WAF (1TiB window)")
axR.plot(xs, waf_cum, color="#d62728", lw=1.0, ls="--", alpha=0.6, label="cold WAF (cumulative)")
axR.set_ylabel("cold (QLC) WAF  /  waf_w", color="#d62728")
axR.tick_params(axis="y", labelcolor="#d62728")
axR.set_ylim(bottom=1.0)

lns = axL.get_lines() + axR.get_lines()
axL.legend(lns, [l.get_label() for l in lns], loc="upper left", fontsize=8, framealpha=0.9)
plt.title("hi REFLASH: U_B and cold WAF vs host writes")
fig.tight_layout()
fig.savefig("A_ub_waf_hi.pdf")
fig.savefig("A_ub_waf_hi.png", dpi=130)
print("rows=%d  x=[%.2f,%.2f]TiB  U_B last=%.3f  WAFcum last=%.3f  WAFwin last=%.3f"
      % (len(xs), xs[0], xs[-1], ub[-1], waf_cum[-1], waf_win[-1]))
