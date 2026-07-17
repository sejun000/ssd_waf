#!/usr/bin/env python3
"""Per-segs panels of TEC_t(u+δ) vs TEC_{t+δ}(u) over time for v4 EWMA r=8.64
sweep. TEC = r·F + G; predictor at t emits TEC(u+δ), compared with realized
TEC at t+δ.  WIN=64 rows fixed box MA.
"""
import os, re
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

PERIODS = [1, 2, 4, 8, 16, 32, 64, 128]
TAG_FMT_864 = "LOG_GREEDY_COST_BENEFIT_10_GS_us02_ewma_hl1572864_gsv4_segs{p}.stat_pr864"
TAG_FMT_8   = "LOG_GREEDY_COST_BENEFIT_10_GS_us02_ewma_hl1572864_gsv4_segs{p}.stat_pr8"
KEY_RE  = re.compile(r"(\w+):?\s+(-?\d+(?:\.\d+)?)")
TIB     = 1024**4
WIN     = 4

def find_stat(p):
    p864 = TAG_FMT_864.format(p=p)
    if os.path.exists(p864): return p864, 8.64
    p8 = TAG_FMT_8.format(p=p)
    if os.path.exists(p8): return p8, 8.0
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

def smooth(y, win=WIN):
    if len(y) < win: return y
    return np.convolve(y, np.ones(win)/win, mode="same")

def trim_leading_zero(*arrays):
    sig = np.zeros(len(arrays[0]), dtype=bool)
    for a in arrays: sig |= (a != 0)
    if not sig.any(): return [a[:0] for a in arrays]
    return [a[np.argmax(sig):] for a in arrays]

fig, axes = plt.subplots(4, 2, figsize=(14, 14), sharex=True)
for ax, p in zip(axes.ravel(), PERIODS):
    path, r_used = find_stat(p)
    if path is None:
        ax.text(0.5,0.5,f"missing segs={p}",ha="center",va="center",transform=ax.transAxes); continue
    hw, gu, gud, fu, fud = parse(path)
    hw, gu, gud, fu, fud = trim_leading_zero(hw, gu, gud, fu, fud)
    if len(hw) <= max(p, WIN):
        ax.text(0.5,0.5,"not enough rows",ha="center",va="center",transform=ax.transAxes); continue
    tec_pred = r_used*fud + gud
    tec_real = r_used*fu  + gu
    tp_s = smooth(tec_pred)
    tr_s = smooth(tec_real)
    t = hw[:-p]
    pred = tp_s[:-p]
    real = tr_s[p:]
    # restrict to 6 ≤ t ≤ 14 TiB
    m = (t >= 6) & (t <= 14)
    t, pred, real = t[m], pred[m], real[m]
    ax.plot(t, pred, color="#4C72B0", lw=1.3, label=f"TEC_t(u+δ)  pred  (r={r_used})")
    ax.plot(t, real, color="#DD8452", lw=1.3, label="TEC_(t+δ)(u)  realized")
    row_dt = hw[1]-hw[0] if len(hw)>1 else 0
    ax.set_title(f"segs={p}  (δ={p} rows = {p*row_dt:.3f} TiB)", fontsize=11)
    ax.set_ylabel("TEC rate", fontsize=10)
    ax.grid(alpha=0.3)
    ax.legend(loc="upper right", fontsize=8)
for ax in axes[-1]:
    ax.set_xlabel("host write t (TiB)", fontsize=10)
fig.suptitle(f"v4 EWMA: TEC_t(u+δ)=r·F(u+δ)+G(u+δ)  vs  TEC_(t+δ)(u)=r·F(u)+G(u)  (smooth WIN={WIN} rows)", fontsize=12)
fig.tight_layout()
for ext in ("pdf","png"):
    out = f"gs_v4_tec_pred_vs_real_w{WIN}_6to14_r864.{ext}"
    fig.savefig(out, bbox_inches="tight")
    print(f"wrote {out}")
