#!/usr/bin/env python3
"""TEC level error (host write included).
For each row t:
  TEC(t)        = host_write_blocks(t) + comp(t) + r·evict(t)
  TEC_pred(t+δ) = TEC(t) + ΔH·(1 + G_u_delta(t) + r·F_u_delta(t))
  TEC_real(t+δ) = host_write_blocks(t+δ) + comp(t+δ) + r·evict(t+δ)
Errors:
  cumulative err = (TEC_pred(t+δ) − TEC_real(t+δ)) / TEC_real(t+δ)
  δ-window  err = (TEC_pred − TEC_real)_added_only / (TEC_real_added_only)
                 where added = next-δ portion only.
Window: 6~14 TiB host write. Reports mean signed / |err|.
"""
import os, re
import numpy as np

KEY_RE = re.compile(r"(\w+):?\s+(-?\d+(?:\.\d+)?)")
TIB    = 1024**4
BLK    = 4096
PERIODS = [1, 2, 4, 8, 16, 32, 64, 128]
T_LO, T_HI = 6.0, 14.0

def find_stat(p):
    for rtag, r in [(864, 8.64), (8, 8.0)]:
        path = f"LOG_GREEDY_COST_BENEFIT_10_GS_us02_ewma_hl1572864_gsv4_segs{p}.stat_pr{rtag}"
        if os.path.exists(path) and os.path.getsize(path) > 0: return path, r
    return None, None

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
                             float(kv["G_u_delta"]),
                             float(kv["F_u_delta"])))
            except (KeyError, ValueError): continue
    return np.array(rows, dtype=float)

print(f"6~14 TiB window, TEC = H + comp + r·evict (4KB blocks).  ΔH = host_write_blocks added over δ rows.")
print(f"pred(t+δ) = TEC(t) + ΔH·(1 + G_uδ(t) + r·F_uδ(t))")
print()
print(f"{'segs':>4} {'r':>5} {'n':>5}  |  {'cum_mean':>10} {'cum_mae':>10}  |  {'δwin_mean':>10} {'δwin_mae':>10} {'pred/real':>10}")
print("-" * 92)
for p in PERIODS:
    path, r = find_stat(p)
    if path is None: continue
    a = parse(path)
    if len(a) <= p+1: continue
    hw_b = a[:,0]; comp = a[:,1]; evict = a[:,2]
    Gud  = a[:,3]; Fud  = a[:,4]
    H    = hw_b / BLK
    t    = hw_b / TIB

    TEC      = H + comp + r*evict
    dH       = H[p:] - H[:-p]
    pred_add = dH * (1.0 + Gud[:-p] + r*Fud[:-p])
    real_add = (comp[p:] - comp[:-p]) + r*(evict[p:] - evict[:-p]) + dH

    TEC_pred = TEC[:-p] + pred_add
    TEC_real = TEC[p:]

    tt = t[:-p]
    mask = (tt >= T_LO) & (tt <= T_HI) & (real_add > 0) & (TEC_real > 0)
    if mask.sum() < 5: continue

    err_cum  = (TEC_pred[mask] - TEC_real[mask]) / TEC_real[mask]
    err_win  = (pred_add[mask] - real_add[mask]) / real_add[mask]

    print(f"{p:>4} {r:>5.2f} {int(mask.sum()):>5}  |  {err_cum.mean():>+10.5f} {np.abs(err_cum).mean():>10.5f}  |  {err_win.mean():>+10.4f} {np.abs(err_win).mean():>10.4f} {pred_add[mask].mean()/real_add[mask].mean():>10.4f}")
