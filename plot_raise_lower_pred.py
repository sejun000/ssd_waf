#!/usr/bin/env python3
"""Per-decision (raise / lower) prediction accuracy on segs=2 v4 stat.
At each row t:
  ΔTEC_pred = (G_u_delta + r·F_u_delta) − (G_u + r·F_u)        [ratio-units]
  ΔTEC_real = TEC_rate[t→t+δ] − TEC_rate[t−δ→t]                [ratio-units]
where TEC_rate over a δ-row interval = (Δcomp + r·Δevict)/Δhw_blocks.

Decisions:
  raise : target_valid_rate[t] > target_valid_rate[t-1]
  lower : target_valid_rate[t] < target_valid_rate[t-1]

Filter: t in 6..14 TiB host write.
"""
import os, re
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

KEY_RE = re.compile(r"(\w+):?\s+(-?\d+(?:\.\d+)?)")
TIB    = 1024**4
BLK    = 4096
R      = 8.64
DELTA  = 2          # segs=2
T_LO, T_HI = 6.0, 14.0
PATH = "LOG_GREEDY_COST_BENEFIT_10_GS_us02_ewma_hl1572864_gsv4_segs2.stat_pr864"

def parse(path):
    rows = []
    with open(path) as fh:
        for line in fh:
            if not line.startswith("LOG_GREEDY"): continue
            kv = dict(KEY_RE.findall(line))
            try:
                rows.append((int(kv["write_size_to_cache"]),
                             int(kv["compacted_blocks"]),
                             int(kv["evicted_blocks"]),
                             float(kv["G_u"]),
                             float(kv["F_u"]),
                             float(kv["G_u_delta"]),
                             float(kv["F_u_delta"]),
                             float(kv["target_valid_rate"])))
            except (KeyError, ValueError): continue
    return np.array(rows, dtype=float)

a = parse(PATH)
hw_b = a[:,0]; comp = a[:,1]; evict = a[:,2]
Gu = a[:,3]; Fu = a[:,4]; Gud = a[:,5]; Fud = a[:,6]
target = a[:,7]
hw_TiB = hw_b / TIB
d = DELTA

# δ-window TEC rate (per host-write block)
def rate(start, end):
    dh = (hw_b[end] - hw_b[start]) / BLK
    if dh <= 0: return np.nan
    return ((comp[end]-comp[start]) + R*(evict[end]-evict[start])) / dh

# precompute rates for each t
rate_back = np.full(len(a), np.nan)  # [t-δ, t]
rate_fwd  = np.full(len(a), np.nan)  # [t, t+δ]
for t in range(d, len(a)-d):
    rate_back[t] = rate(t-d, t)
    rate_fwd[t]  = rate(t, t+d)

dtec_pred = (Gud + R*Fud) - (Gu + R*Fu)
dtec_real = rate_fwd - rate_back

# decisions
tgt_diff = np.zeros(len(a))
tgt_diff[1:] = target[1:] - target[:-1]
is_raise = tgt_diff > 1e-9
is_lower = tgt_diff < -1e-9

# window mask
mask_t = (hw_TiB >= T_LO) & (hw_TiB <= T_HI) & ~np.isnan(dtec_real) & ~np.isnan(dtec_pred)
m_r = mask_t & is_raise
m_l = mask_t & is_lower

def stats(p, r, label):
    if len(p) == 0:
        print(f"{label:>6}: n=0")
        return
    sgn = np.sign(p) == np.sign(r)
    print(f"{label:>6}: n={len(p):>4}  "
          f"pred_mean={p.mean():+.5f}  real_mean={r.mean():+.5f}  "
          f"sign_match={sgn.mean()*100:.1f}%  "
          f"corr={np.corrcoef(p,r)[0,1]:+.3f}")

print(f"segs={d}, r={R}, window {T_LO}-{T_HI} TiB")
print(f"total decisions in window: raise={m_r.sum()}, lower={m_l.sum()}, stay={(mask_t & ~is_raise & ~is_lower).sum()}")
stats(dtec_pred[m_r], dtec_real[m_r], "raise")
stats(dtec_pred[m_l], dtec_real[m_l], "lower")
print()

# Plot: 2x2  (raise scatter, lower scatter, raise time, lower time)
fig, axes = plt.subplots(2, 2, figsize=(13, 9))
def scatter(ax, m, color, title):
    p = dtec_pred[m]; r = dtec_real[m]
    ax.scatter(p, r, s=15, alpha=0.55, color=color)
    lim = max(np.abs(p).max() if len(p) else 1, np.abs(r).max() if len(r) else 1) * 1.05
    ax.plot([-lim, lim], [-lim, lim], "k--", lw=0.7, alpha=0.5)
    ax.axhline(0, color="gray", lw=0.4); ax.axvline(0, color="gray", lw=0.4)
    ax.set_xlabel("ΔTEC_pred = (G_uδ+r·F_uδ) − (G_u+r·F_u)", fontsize=9)
    ax.set_ylabel("ΔTEC_real = rate[t,t+δ] − rate[t−δ,t]", fontsize=9)
    if len(p):
        sm = (np.sign(p) == np.sign(r)).mean()*100
        ax.set_title(f"{title}  (n={len(p)}, sign_match={sm:.1f}%)", fontsize=10)
    else:
        ax.set_title(f"{title}  (n=0)", fontsize=10)
    ax.grid(alpha=0.3)

def tseries(ax, m, color, title):
    t = hw_TiB[m]; p = dtec_pred[m]; r = dtec_real[m]
    ax.plot(t, p, "o", ms=3, alpha=0.6, color="#4C72B0", label="pred ΔTEC")
    ax.plot(t, r, "x", ms=3, alpha=0.6, color="#DD8452", label="real ΔTEC")
    ax.axhline(0, color="gray", lw=0.5)
    ax.set_xlabel("host write t (TiB)", fontsize=9)
    ax.set_ylabel("ΔTEC", fontsize=9)
    ax.set_title(title, fontsize=10)
    ax.legend(fontsize=8, loc="upper right")
    ax.grid(alpha=0.3)

scatter(axes[0,0], m_r, "#4C72B0", "RAISE — pred vs real")
scatter(axes[0,1], m_l, "#DD8452", "LOWER — pred vs real")
tseries(axes[1,0], m_r, "#4C72B0", "RAISE timeline")
tseries(axes[1,1], m_l, "#DD8452", "LOWER timeline")

fig.suptitle(f"segs={d}, r={R}: prediction accuracy at raise/lower decisions (6~14 TiB)", fontsize=12)
fig.tight_layout()
for ext in ("pdf","png"):
    out = f"gs_v4_raise_lower_pred_segs{d}.{ext}"
    fig.savefig(out, bbox_inches="tight"); print(f"wrote {out}")
