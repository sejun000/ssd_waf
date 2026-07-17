#!/usr/bin/env python3
"""Compare GS r=8.64 trace against cb_11_dwpd2 oracle.
For each GS row i:
  target_U(i) → nearest dp.0.U file (CB_11 oracle at fixed U_b=U).
  Interpolate oracle cumulative comp/evict at GS row's write_size_to_cache.
WIN=32 rolling delta smoothing applied to both.
Plot: time(TiB) vs compacted-rate (one fig) and flush-rate (another fig),
each with 4 subplots = segs ∈ {1,2,4,8}.  GS actual vs CB oracle overlay.
"""
import os, re, glob
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

KEY_RE   = re.compile(r"(\w+):?\s+(-?\d+(?:\.\d+)?)")
TIB      = 1024**4
BLK      = 4096
PERIODS  = [1, 2, 4, 8, 16]
WIN_GS   = 64    # both sides smoothed equally (fair like-for-like comparison)
WIN_CB   = 64
CB_DIR   = "cb_11_dwpd2"

def parse_gs(path):
    rows = []
    with open(path) as fh:
        for line in fh:
            if not line.startswith("LOG_GREEDY"): continue
            kv = dict(KEY_RE.findall(line))
            try:
                rows.append((int(kv["write_size_to_cache"]),
                             int(kv["compacted_blocks"]),
                             int(kv["evicted_blocks"]),
                             float(kv["target_valid_rate"]),
                             float(kv["G_u_delta"]),
                             float(kv["G_u"]),
                             float(kv["util_step"])))
            except (KeyError, ValueError): continue
    return np.array(rows, dtype=float)

def parse_cb(path):
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
    return np.array(rows, dtype=float)

# Load all CB oracle files keyed by U value
cb_oracle = {}
for path in sorted(glob.glob(os.path.join(CB_DIR, "dp.0.*"))):
    m = re.search(r"dp\.0\.(\d+)$", path)
    if not m: continue
    raw = m.group(1)
    # dp.0.5 → 0.5 (not 0.05); dp.0.45 → 0.45.
    u = float(f"0.{raw}")
    a = parse_cb(path)
    if len(a) > 5:
        cb_oracle[u] = a   # columns: write_size, comp, evict
print(f"loaded {len(cb_oracle)} CB oracle files, U range {min(cb_oracle):.2f}~{max(cb_oracle):.2f}")
oracle_us = np.array(sorted(cb_oracle))

def nearest_u(u):
    if u <= 0: return None
    return float(oracle_us[np.argmin(np.abs(oracle_us - u))])

def find_gs(p):
    # New gsdec864 sweep (with GS_DECISION_LOG enabled); preferred.
    for tag in ("gsdec864",):
        for rtag in (864,):
            path = f"LOG_GREEDY_COST_BENEFIT_10_GS_us02_ewma_hl1572864_{tag}_segs{p}.stat_pr{rtag}"
            if os.path.exists(path) and os.path.getsize(path) > 0: return path
    return None

# ---------- compute series per seg ----------
seg_data = {}   # p → dict
for p in PERIODS:
    path = find_gs(p)
    if not path:
        print(f"missing GS trace for segs={p}")
        continue
    a = parse_gs(path)
    w_gs = WIN_GS
    w_cb = WIN_CB
    if len(a) <= max(w_gs, w_cb)+1: continue
    ws    = a[:,0]; comp = a[:,1]; evict = a[:,2]; tgt = a[:,3]
    gud   = a[:,4]; gu  = a[:,5]; ustep = a[:,6]
    H     = ws / BLK   # host write in blocks
    t_tib = ws / TIB

    # GS: raw 1-row rate (per-row, GiB/row).  GS's internal EWMA already
    # smooths the policy decision; the cumulative output is the un-smoothed
    # ground truth of how many blocks were compacted/flushed each row.
    gs_c_per_row = (comp[w_gs:]  - comp[:-w_gs])  * BLK / (1024**3) / w_gs
    gs_f_per_row = (evict[w_gs:] - evict[:-w_gs]) * BLK / (1024**3) / w_gs
    t_x_gs       = t_tib[:-w_gs]
    # cumulative columns (for clean 0~14 TiB ratio)
    comp_cum_gs  = comp
    evict_cum_gs = evict

    # CB oracle: at each row i, choose dp.{nearest_u(tgt[i])}, interp cumulative
    n = len(a)
    cb_comp_cum  = np.full(n, np.nan)
    cb_evict_cum = np.full(n, np.nan)
    for i in range(n):
        u = nearest_u(tgt[i])
        if u is None: continue
        ora = cb_oracle[u]
        cb_comp_cum[i]  = np.interp(ws[i], ora[:,0], ora[:,1], left=ora[0,1], right=ora[-1,1])
        cb_evict_cum[i] = np.interp(ws[i], ora[:,0], ora[:,2], left=ora[0,2], right=ora[-1,2])

    # CB: w_cb-row smoothed per-row rate (GiB/row averaged over w_cb rows)
    cb_c_per_row = (cb_comp_cum[w_cb:]  - cb_comp_cum[:-w_cb])  * BLK / (1024**3) / w_cb
    cb_f_per_row = (cb_evict_cum[w_cb:] - cb_evict_cum[:-w_cb]) * BLK / (1024**3) / w_cb
    t_x_cb       = t_tib[:-w_cb]

    # align by time: clip to shared length so plotting/ratio matches at same i
    n = min(len(gs_c_per_row), len(cb_c_per_row))
    gs_c = gs_c_per_row[:n]; gs_f = gs_f_per_row[:n]
    cb_c = cb_c_per_row[:n]; cb_f = cb_f_per_row[:n]
    t_x  = t_x_gs[:n]
    tgt_x = tgt[:n]

    # Means over 0~14 TiB
    in_win_full = (t_tib >= 0) & (t_tib <= 14)
    gud_mean    = np.mean(gud[in_win_full]) if in_win_full.any() else float('nan')
    gu_mean     = np.mean(gu[in_win_full])  if in_win_full.any() else float('nan')
    gs_diff_mean = np.mean((gud - gu)[in_win_full]) if in_win_full.any() else float('nan')

    # Oracle G(u) at every row from cb_comp_cum (rolling per-row rate over w_cb)
    dH_cb    = H[w_cb:] - H[:-w_cb]
    dcomp_cb = cb_comp_cum[w_cb:] - cb_comp_cum[:-w_cb]
    with np.errstate(divide='ignore', invalid='ignore'):
        oracle_g_rate = np.where(dH_cb > 0, dcomp_cb / dH_cb, np.nan)
    in_win_cb = (t_tib[:-w_cb] >= 0) & (t_tib[:-w_cb] <= 14) & np.isfinite(oracle_g_rate)
    oracle_g_mean = np.nanmean(oracle_g_rate[in_win_cb]) if in_win_cb.any() else float('nan')

    # Oracle G(u + util_step) by re-interpolating cumulative on the SHIFTED U
    n = len(a)
    cb_comp_cum_shift = np.full(n, np.nan)
    for i in range(n):
        u_shift = nearest_u(tgt[i] + ustep[i])
        if u_shift is None: continue
        ora = cb_oracle[u_shift]
        cb_comp_cum_shift[i] = np.interp(ws[i], ora[:,0], ora[:,1],
                                         left=ora[0,1], right=ora[-1,1])
    dcomp_cb_s = cb_comp_cum_shift[w_cb:] - cb_comp_cum_shift[:-w_cb]
    with np.errstate(divide='ignore', invalid='ignore'):
        oracle_gd_rate = np.where(dH_cb > 0, dcomp_cb_s / dH_cb, np.nan)
    in_win_cb_s = (t_tib[:-w_cb] >= 0) & (t_tib[:-w_cb] <= 14) & np.isfinite(oracle_gd_rate)
    oracle_gd_mean = np.nanmean(oracle_gd_rate[in_win_cb_s]) if in_win_cb_s.any() else float('nan')
    # Oracle diff = G(u+δ) - G(u) at matched rows
    common = in_win_cb & in_win_cb_s & np.isfinite(oracle_gd_rate)
    oracle_diff_mean = np.nanmean((oracle_gd_rate - oracle_g_rate)[common]) if common.any() else float('nan')

    # mean target U_B over [0, 14] TiB
    tgt_mean = float(np.mean(tgt[in_win_full])) if in_win_full.any() else float('nan')

    seg_data[p] = dict(t=t_x, gs_c=gs_c, gs_f=gs_f,
                       cb_c=cb_c,  cb_f=cb_f,
                       tgt=tgt_x, win_gs=w_gs, win_cb=w_cb,
                       ws=ws, t_full=t_tib,
                       comp_cum_gs=comp_cum_gs, evict_cum_gs=evict_cum_gs,
                       cb_comp_cum=cb_comp_cum, cb_evict_cum=cb_evict_cum,
                       gud_mean=gud_mean, gu_mean=gu_mean,
                       gs_diff_mean=gs_diff_mean,
                       oracle_g_mean=oracle_g_mean,
                       oracle_gd_mean=oracle_gd_mean,
                       oracle_diff_mean=oracle_diff_mean,
                       tgt_mean=tgt_mean)

# ---------- plot ----------
def plot_metric(key_gs, key_cb, title, ylab, outname):
    fig, axes = plt.subplots(2, 2, figsize=(13, 8.5), sharex=True)
    for ax, p in zip(axes.ravel(), PERIODS):
        if p not in seg_data:
            ax.text(0.5, 0.5, f"missing segs={p}", ha="center", va="center", transform=ax.transAxes)
            continue
        d = seg_data[p]
        m = (d['t'] >= 0) & (d['t'] <= 14)
        ax.plot(d['t'][m], d[key_gs][m], color="C0", lw=1.1, label="GS actual")
        ax.plot(d['t'][m], d[key_cb][m], color="C3", lw=1.1, alpha=0.85, label="CB oracle @ GS target_U")
        ax.set_title(f"segs={p}  (GS WIN={d['win_gs']}, CB WIN={d['win_cb']})", fontsize=10)
        ax.set_ylabel(ylab, fontsize=9)
        ax.grid(alpha=0.3)
        ax.legend(fontsize=8, loc="upper right")
    for ax in axes[-1]:
        ax.set_xlabel("host write t (TiB)", fontsize=10)
    fig.suptitle(f"{title}  (GS raw per-row vs CB rolling {WIN_CB}-row avg, r=8.64)", fontsize=12)
    fig.tight_layout()
    for ext in ("pdf",):
        out = f"A_{outname}.{ext}"
        fig.savefig(out, bbox_inches="tight"); print(f"wrote {out}")
    plt.close(fig)

plot_metric("gs_c", "cb_c",
            "Compacted blocks per WIN rows: GS r=8.64 vs CB_11 oracle@U(t)",
            "Δcompacted (GiB / 32 rows)",
            "gs_vs_cb_compacted_w32")
plot_metric("gs_f", "cb_f",
            "Evicted (flush) blocks per WIN rows: GS r=8.64 vs CB_11 oracle@U(t)",
            "Δevicted (GiB / 32 rows)",
            "gs_vs_cb_flushed_w32")

# ---------- summary ratio per seg ----------
print(f"\nGS WIN={WIN_GS} (raw per-row, internal EWMA already smooths) vs CB WIN={WIN_CB}-row avg.")
print(f"Two ratios reported per seg in 0~14 TiB:")
print(f"  (a) per-row rate ratio    = Σ gs_rate_per_row / Σ cb_rate_per_row")
print(f"  (b) clean cumulative ratio = (comp_GS_end - comp_GS_start)/(comp_CB_end - comp_CB_start)")
print()
print(f"{'segs':>4}  |  {'(a) Σcomp_GS/Σcomp_CB':>22} {'(a) Σflush_GS/Σflush_CB':>24}  |  "
      f"{'(b) cum_comp_ratio':>20} {'(b) cum_flush_ratio':>22}")
for p in PERIODS:
    if p not in seg_data: continue
    d = seg_data[p]
    # (a) overlapping rolling sum on the rolling-delta arrays
    m = (d['t'] >= 0) & (d['t'] <= 14) & np.isfinite(d['cb_c']) & np.isfinite(d['cb_f'])
    r_c = d['gs_c'][m].sum() / d['cb_c'][m].sum() if d['cb_c'][m].sum() > 0 else float("nan")
    r_f = d['gs_f'][m].sum() / d['cb_f'][m].sum() if d['cb_f'][m].sum() > 0 else float("nan")
    # (b) clean cumulative on full series — pick first/last index in [0,14] TiB
    mf = (d['t_full'] >= 0) & (d['t_full'] <= 14) & np.isfinite(d['cb_comp_cum']) & np.isfinite(d['cb_evict_cum'])
    idx = np.where(mf)[0]
    if len(idx) >= 2:
        i0, i1 = idx[0], idx[-1]
        dc_gs = d['comp_cum_gs'][i1]  - d['comp_cum_gs'][i0]
        de_gs = d['evict_cum_gs'][i1] - d['evict_cum_gs'][i0]
        dc_cb = d['cb_comp_cum'][i1]  - d['cb_comp_cum'][i0]
        de_cb = d['cb_evict_cum'][i1] - d['cb_evict_cum'][i0]
        cum_c = dc_gs / dc_cb if dc_cb > 0 else float("nan")
        cum_f = de_gs / de_cb if de_cb > 0 else float("nan")
    else:
        cum_c = cum_f = float("nan")
    print(f"{p:>4}  |  {r_c:>22.4f} {r_f:>24.4f}  |  {cum_c:>20.4f} {cum_f:>22.4f}")

# ============================================================================
# Combined figure: left = oracle-diff bar chart, right = cumulative TEC (norm)
# ============================================================================
R_VAL = 8.64

# bump global fonts
plt.rcParams.update({
    "font.size":       14,
    "axes.labelsize":  15,
    "xtick.labelsize": 13,
    "ytick.labelsize": 13,
    "legend.fontsize": 13,
})

fig, (axL, axR) = plt.subplots(1, 2, figsize=(13, 5.2))

# --- collect per-seg mean target U_B (valid block ratio)
segs_list, tgt_means = [], []
for p in PERIODS:
    if p not in seg_data: continue
    d = seg_data[p]
    if not np.isfinite(d['tgt_mean']): continue
    segs_list.append(p)
    tgt_means.append(d['tgt_mean'])

def seg_label(p):
    return f"{p} seg" if p == 1 else f"{p} segs"

n_segs    = len(segs_list)
seg_colors = plt.cm.viridis(np.linspace(0.15, 0.85, n_segs))

# --- find U* from CB_11: cumulative TEC at 14 TiB minimized over U
def tec_per_host(u, ws_target=14*TIB):
    ora = cb_oracle[u]
    c = np.interp(ws_target, ora[:,0], ora[:,1], left=ora[0,1], right=ora[-1,1])
    e = np.interp(ws_target, ora[:,0], ora[:,2], left=ora[0,2], right=ora[-1,2])
    h = ws_target / BLK
    return (h + c + R_VAL * e) / h
tec_by_u = {u: tec_per_host(u) for u in oracle_us}
u_star = min(tec_by_u, key=tec_by_u.get)
print(f"\nCB_11 optimal fixed U_B = {u_star:.2f}  (TEC/host = {tec_by_u[u_star]:.4f})")

# --- Left: per-seg bars, average target U_B, with reference line at U*
xL = np.arange(n_segs)
for i, p in enumerate(segs_list):
    axL.bar(xL[i], tgt_means[i], 0.6, color=seg_colors[i], edgecolor="k", linewidth=0.5)
    axL.text(xL[i], tgt_means[i] + 0.01, f"{tgt_means[i]:.3f}", ha="center", fontsize=12)
axL.axhline(u_star, color="red", lw=1.2, ls="--",
            label=f"CB_11 optimal U* = {u_star:.2f}")
axL.set_xticks(xL)
axL.set_xticklabels([seg_label(p) for p in segs_list])
axL.set_ylabel("Average target  U_B   (valid blocks / cache blocks)")
axL.grid(alpha=0.3, axis="y")
axL.set_ylim(0.0, max(max(tgt_means), u_star) * 1.15)
axL.legend(loc="lower right", frameon=True)

# --- Right: bar chart of final cumulative TEC, normalized to 2 segs
tec_final = {}
for p in PERIODS:
    if p not in seg_data: continue
    d = seg_data[p]
    H_blocks = d['ws'] / BLK
    tec_blocks = H_blocks + d['comp_cum_gs'] + R_VAL * d['evict_cum_gs']
    m = (d['t_full'] >= 0) & (d['t_full'] <= 14)
    tec_final[p] = tec_blocks[m][-1] if m.any() else np.nan

ref = tec_final.get(2, 1.0)
xs  = np.arange(n_segs)
norm_vals = [tec_final[p] / ref for p in segs_list]
print("\nNormalized cumulative TEC @ 14 TiB (ref = 2 segs):")
for p, v in zip(segs_list, norm_vals):
    print(f"  {seg_label(p):>8}  {v:.4f}")

for i, p in enumerate(segs_list):
    val = norm_vals[i]
    axR.bar(xs[i], val, 0.6, color=seg_colors[i], edgecolor="k", linewidth=0.5)
    axR.text(xs[i], val + 0.004, f"{val:.3f}", ha="center", fontsize=12)
axR.set_xticks(xs)
axR.set_xticklabels([seg_label(p) for p in segs_list])
axR.set_ylabel("Cumulative TEC (normalized to 2 segs)")
axR.axhline(1.0, color="gray", lw=0.6, ls="--")
axR.grid(alpha=0.3, axis="y")
lo = min(norm_vals) - 0.02
hi = max(norm_vals) + 0.03
axR.set_ylim(max(0.0, lo), hi)

fig.tight_layout()
for ext in ("pdf",):
    out = f"A_gs_oracle_diff_and_tec.{ext}"
    fig.savefig(out, bbox_inches="tight"); print(f"wrote {out}")
plt.close(fig)
