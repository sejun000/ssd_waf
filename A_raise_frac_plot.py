#!/usr/bin/env python3
# hi REFLASH: raise(GC) fraction vs waf_w.  (reconstructed: raise <=> target_valid_rate > U_B)
import json, math
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt

d = json.load(open("A_raise_frac.json"))
hw, ww, rf = d["hw"], d["wafw"], d["rf"]

# rolling mean of raise_frac (~1 TiB ~ 10.7 bins of 96GiB)
K = 11
rfs = []
for i in range(len(rf)):
    a = max(0, i-K//2); b = min(len(rf), i+K//2+1)
    rfs.append(sum(rf[a:b])/(b-a))

fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(13, 5), gridspec_kw={"width_ratios":[2,1]})

# --- time series ---
ax1.plot(hw, rf, color="#7f7f7f", lw=0.6, alpha=0.45, label="raise_frac (96GiB bin)")
ax1.plot(hw, rfs, color="#1f77b4", lw=2.0, label="raise_frac (rolling ~1TiB)")
ax1.axhline(0.5, color="k", lw=0.7, ls=":", alpha=0.5)
ax1.set_xlabel("host writes (TiB)"); ax1.set_ylabel("raise(GC) fraction", color="#1f77b4")
ax1.set_ylim(0, 1.05); ax1.tick_params(axis="y", labelcolor="#1f77b4"); ax1.grid(alpha=0.25)
axr = ax1.twinx()
axr.plot(hw, ww, color="#2ca02c", lw=1.6, label="waf_w")
axr.set_ylabel("waf_w (policy input)", color="#2ca02c"); axr.tick_params(axis="y", labelcolor="#2ca02c")
axr.axvline(24, color="#d62728", lw=1.0, ls="--", alpha=0.7)
axr.text(24.3, axr.get_ylim()[1]*0.95, "WAF onset ~24T", color="#d62728", fontsize=8, va="top")
l1 = ax1.get_lines() + axr.get_lines()
ax1.legend(l1, [l.get_label() for l in l1], loc="lower right", fontsize=8)
ax1.set_title("raise(GC) fraction & waf_w vs host writes")

# --- scatter post-onset ---
xs = [ww[i] for i in range(len(hw)) if hw[i] >= 25]
ys = [rf[i] for i in range(len(hw)) if hw[i] >= 25]
ax2.scatter(xs, ys, s=8, alpha=0.35, color="#1f77b4", edgecolors="none")
# linear fit
n = len(xs); mx = sum(xs)/n; my = sum(ys)/n
sxy = sum((xs[i]-mx)*(ys[i]-my) for i in range(n)); sxx = sum((x-mx)**2 for x in xs)
syy = sum((y-my)**2 for y in ys)
b = sxy/sxx; a = my - b*mx; r = sxy/math.sqrt(sxx*syy)
xr = [min(xs), max(xs)]
ax2.plot(xr, [a+b*x for x in xr], color="#d62728", lw=2, label="fit: r=%.2f, slope=%.3f" % (r, b))
ax2.set_xlabel("waf_w"); ax2.set_ylabel("raise(GC) fraction (96GiB bin)")
ax2.set_title("post-onset (>=25T): waf_w vs raise_frac")
ax2.set_ylim(0, 1.05); ax2.grid(alpha=0.25); ax2.legend(fontsize=8, loc="lower right")

fig.tight_layout()
fig.savefig("A_raise_frac.pdf"); fig.savefig("A_raise_frac.png", dpi=130)
print("saved. pre-onset rf avg=%.3f post-onset rf avg=%.3f  scatter r=%.3f slope=%.3f" %
      (sum(rf[i] for i in range(len(hw)) if 8<=hw[i]<23)/sum(1 for i in range(len(hw)) if 8<=hw[i]<23),
       my, r, b))
