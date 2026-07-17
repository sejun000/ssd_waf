#!/usr/bin/env python3
"""Per-segs (1..128) plot: mean TEC relative error + final normalized TEC.
TEC_t(u+δ)        = r·F(u+δ) + G(u+δ)        ← prediction at time t
TEC_{t+δ}(u)      = r·F_{t+δ}(u) + G_{t+δ}(u) ← realized at time t+δ
err = (TEC_pred − TEC_real) / TEC_real
"""
import os, re, pandas as pd
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

KEY_RE = re.compile(r"(\w+):?\s+(-?\d+(?:\.\d+)?)")
TIB    = 1024**4
BLOCK  = 4096
R      = 8.64
SIX_TIB_BYTES = 6 * TIB
PERIODS = [1, 2, 4, 8, 16, 32, 64, 128]

def find_stat(p):
    for rtag, r_used in [(864, 8.64), (8, 8.0)]:
        path = f"LOG_GREEDY_COST_BENEFIT_10_GS_us02_ewma_hl1572864_gsv4_segs{p}.stat_pr{rtag}"
        if os.path.exists(path):
            return path, r_used
    return None, None

def parse_rates(path):
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

def parse_tec_blocks(path):
    """Final cumulative compacted/evicted (in MB) from 6 TiB to end."""
    rows = []
    with open(path) as fh:
        for line in fh:
            if not line.startswith("LOG_GREEDY"): continue
            kv = dict(KEY_RE.findall(line))
            try:
                rows.append((int(kv["write_size_to_cache"]),
                             int(kv["compacted_blocks"]),
                             int(kv["evicted_blocks"])))
            except (KeyError, ValueError): continue
    if not rows: return None
    idx6 = next((i for i,(hw,_,_) in enumerate(rows) if hw >= SIX_TIB_BYTES), None)
    if idx6 is None: return None
    hw0,c0,e0 = rows[idx6]; hw1,c1,e1 = rows[-1]
    return (c1-c0) * 4096 / 1024**2, (e1-e0) * 4096 / 1024**2

# CSAL baseline TEC (from CSV)
df = pd.read_csv("ftl0_20260225_082514.csv")
df["hw_TiB"] = df["host_write_MB"] / 1024**2
i6 = (df["hw_TiB"] - 6).abs().idxmin()
csal_tec_mb = R * (df.iloc[-1]["backend_write_MB"] - df.iloc[i6]["backend_write_MB"])

mean_err = []; mae = []; tec_norm = []; segs_used = []
for p in PERIODS:
    path, r_used = find_stat(p)
    if path is None: continue
    hw, gu, gud, fu, fud = parse_rates(path)
    sig = (gu!=0)|(gud!=0)|(fu!=0)|(fud!=0)
    if not sig.any(): continue
    i0 = np.argmax(sig)
    hw, gu, gud, fu, fud = hw[i0:], gu[i0:], gud[i0:], fu[i0:], fud[i0:]
    if len(hw) <= p+1: continue
    tec_pred = r_used*fud + gud           # at t
    tec_real = r_used*fu  + gu            # at t (shifted later)
    pred = tec_pred[:-p]
    real = tec_real[p:]
    mask = np.abs(real) > 1e-6
    err = (pred[mask] - real[mask]) / real[mask]
    mean_err.append(np.mean(err))
    mae.append(np.mean(np.abs(err)))

    blocks = parse_tec_blocks(path)
    if blocks is None: continue
    comp_mb, ev_mb = blocks
    tec_mb = comp_mb + r_used * ev_mb
    tec_norm.append(tec_mb / csal_tec_mb)
    segs_used.append(p)

print(f"{'segs':>4} {'mean_err':>10} {'mean|err|':>10} {'TEC/CSAL':>10}")
print("-"*40)
for p, me, ma, tn in zip(segs_used, mean_err, mae, tec_norm):
    print(f"{p:>4} {me:>+10.4f} {ma:>10.4f} {tn:>10.3f}")

# Plot
fig, ax1 = plt.subplots(figsize=(10,5.5))
x = np.array(segs_used, dtype=float)
ax1.plot(x, mae, "o-", color="#4C72B0", lw=1.6, ms=8, label="mean |err| of TEC_t(u+δ) vs TEC_{t+δ}(u)")
ax1.plot(x, mean_err, "s--", color="#4C72B0", lw=1.0, ms=6, alpha=0.55, label="mean signed err")
ax1.axhline(0, color="gray", lw=0.7, ls=":")
ax1.set_xscale("log", base=2)
ax1.set_xticks(x); ax1.set_xticklabels([str(int(v)) for v in x])
ax1.set_xlabel("decision period (segs)")
ax1.set_ylabel("relative err  (pred − real) / real", color="#4C72B0")
ax1.tick_params(axis="y", labelcolor="#4C72B0")
ax1.grid(alpha=0.3)

ax2 = ax1.twinx()
ax2.plot(x, tec_norm, "^-", color="#DD8452", lw=1.6, ms=9, label="TEC / CSAL  (6 TiB → end)")
ax2.axhline(1.0, color="#DD8452", lw=0.7, ls=":")
ax2.set_ylabel("TEC / CSAL", color="#DD8452")
ax2.tick_params(axis="y", labelcolor="#DD8452")

# combined legend
lines, labels = ax1.get_legend_handles_labels()
lines2, labels2 = ax2.get_legend_handles_labels()
ax1.legend(lines+lines2, labels+labels2, loc="upper left", fontsize=9)

ax1.set_title(f"r=8.64: TEC prediction err vs realized normalized TEC", fontsize=12)
fig.tight_layout()
for ext in ("pdf","png"):
    out = f"tec_err_vs_tec_r864.{ext}"
    fig.savefig(out, bbox_inches="tight")
    print(f"wrote {out}")
