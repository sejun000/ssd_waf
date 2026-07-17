#!/usr/bin/env python3
"""Decision sign accuracy for v4 EWMA r=8.64 sweep.

For each stat row at time t:
    pred_sign = sign( r·(F_u[t] − F_u_delta[t]) − (G_u_delta[t] − G_u[t]) )
At t+δ (δ = segs rows):
    real_sign = sign( r·(F_u[t+δ] − F_u_delta[t+δ]) − (G_u_delta[t+δ] − G_u[t+δ]) )
sign_acc[i] = (pred_sign[i] == real_sign[i+δ])
Reports per-segs mean accuracy + plot.
"""
import os, re
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

KEY_RE  = re.compile(r"(\w+):?\s+(-?\d+(?:\.\d+)?)")
TIB     = 1024**4
R       = 8.64
PERIODS = [1, 2, 4, 8, 16, 32, 64, 128]

def find_stat(p):
    for rtag, r_used in [(864, 8.64), (8, 8.0)]:
        path = f"LOG_GREEDY_COST_BENEFIT_10_GS_us02_ewma_hl1572864_gsv4_segs{p}.stat_pr{rtag}"
        if os.path.exists(path):
            return path, r_used
    return None, None

def parse(path):
    hw, gu, gud, fu, fud = [], [], [], [], []
    with open(path) as fh:
        for line in fh:
            if not line.startswith("LOG_GREEDY"): continue
            kv = dict(KEY_RE.findall(line))
            try:
                hw .append(int(kv["write_size_to_cache"]) / TIB)
                gu .append(float(kv["G_u"]))
                gud.append(float(kv["G_u_delta"]))
                fu .append(float(kv["F_u"]))
                fud.append(float(kv["F_u_delta"]))
            except (KeyError, ValueError): continue
    return tuple(np.array(a) for a in (hw, gu, gud, fu, fud))

def smooth(y, win):
    if win < 2 or len(y) < win: return y
    return np.convolve(y, np.ones(win)/win, mode="same")

segs_used = []
raw_acc   = []
pred_raise = []     # fraction of points where pred says raise
real_raise = []    # fraction of points where realized says raise
acc_smooth = []    # accuracy after smoothing both sides
WIN_S = 16

print(f"r=8.64 hill-climb sign accuracy  (WIN_S={WIN_S} rows smooth, raw)")
print(f"{'segs':>4} {'n':>5} {'raw_acc':>9} {'smooth_acc':>11} {'pred_raise':>11} {'real_raise':>11}")
print("-"*70)

for p in PERIODS:
    path, r_used = find_stat(p)
    if path is None: continue
    hw, gu, gud, fu, fud = parse(path)
    sig = (gu!=0)|(gud!=0)|(fu!=0)|(fud!=0)
    if not sig.any(): continue
    i0 = np.argmax(sig)
    hw, gu, gud, fu, fud = hw[i0:], gu[i0:], gud[i0:], fu[i0:], fud[i0:]
    if len(hw) <= p+1: continue

    # raw decision delta at every row: D[i] = r·(F_u − F_u_delta) − (G_u_delta − G_u)
    D = r_used*(fu - fud) - (gud - gu)
    pred_sign = np.sign(D[:-p])
    real_sign = np.sign(D[p:])
    acc       = np.mean(pred_sign == real_sign)

    # smoothed sides for noise-reduced version
    fu_s, fud_s = smooth(fu, WIN_S), smooth(fud, WIN_S)
    gu_s, gud_s = smooth(gu, WIN_S), smooth(gud, WIN_S)
    D_s = r_used*(fu_s - fud_s) - (gud_s - gu_s)
    acc_s = np.mean(np.sign(D_s[:-p]) == np.sign(D_s[p:]))

    pred_raise.append(np.mean(pred_sign > 0))
    real_raise.append(np.mean(real_sign > 0))
    raw_acc.append(acc)
    acc_smooth.append(acc_s)
    segs_used.append(p)
    print(f"{p:>4} {len(pred_sign):>5} {acc:>9.3f} {acc_s:>11.3f} {pred_raise[-1]:>11.3f} {real_raise[-1]:>11.3f}")

# Plot
fig, ax = plt.subplots(figsize=(10,5.5))
x = np.array(segs_used, dtype=float)
ax.plot(x, raw_acc,    "o-", color="#4C72B0", lw=1.6, ms=8, label="raw sign accuracy")
ax.plot(x, acc_smooth, "s--", color="#DD8452", lw=1.4, ms=7, label=f"smoothed (WIN={WIN_S}) sign accuracy")
ax.axhline(0.5, color="gray", lw=0.7, ls=":", label="random baseline")
ax.set_xscale("log", base=2)
ax.set_xticks(x); ax.set_xticklabels([str(int(v)) for v in x])
ax.set_xlabel("decision period (segs)", fontsize=11)
ax.set_ylabel("P(pred sign at t == real sign at t+δ)", fontsize=11)
ax.set_ylim(0, 1)
ax.grid(alpha=0.3)
ax.legend(loc="lower left", fontsize=10)
ax.set_title("r=8.64: hill-climb decision sign accuracy  (pred at t vs realized at t+δ)")
fig.tight_layout()
for ext in ("pdf","png"):
    out = f"gs_v4_sign_accuracy_r864.{ext}"
    fig.savefig(out, bbox_inches="tight")
    print(f"wrote {out}")
