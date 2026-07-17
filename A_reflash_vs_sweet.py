#!/usr/bin/env python3
# For each trace: U_B sweep normalized-TEC curve + sweet spot + REFLASH landing point.
# 3 subplots (dwpd2, dwpd3, ssdtr — dwpd1 not yet finished).
import glob, re, os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

QLC_FACTOR = 8.64
PAGE = 4096
TIB = 1099511627776  # 2^40
FREE_SEG_BYTES = 42 * (1 << 30)  # add 42 GiB for free segments to the U_B denominator

TRACES = [
    # Keep original label order: Ali1=dwpd2_5x, Ali2=dwpd1to2, Ali3=dwpd01to1, SSDtrace4x=scaled4x.
    # label shown    dp_dir                                          stat_tag       gsdec_tag
    ("Alibaba1",     "/home/sejun000/ssd_waf/ali1_dp_cb11",          "dwpd2_5x",    "dwpd2_5x"),
    ("Alibaba2",     "/home/sejun000/ssd_waf/ali2_dp_cb11",          "dwpd1to2",    "dwpd1to2"),
    ("Alibaba3",     "/home/sejun000/ssd_waf/ali3_dp",               "dwpd01to1",   "dwpd01to1"),
    ("YCSB-A",       "/home/sejun000/ssd_waf/ssdtrace_dp",           "scaled4x",    "scaled4x"),
]

WARMUP_TIB = 6

UB_AVG_WARMUP_TIB = 6  # skip first 6 TiB fill phase when averaging actual_UB

def parse_dp_sweep(dp_dir):
    """Return [(avg_actual_UB, host_TiB, comp_TiB, evict_TiB, raw_TEC), ...] sorted by UB.

    x-axis is the post-fill mean of global_valid_blocks / cache_blocks across rows
    within each dp.* file (one point per 1% sweep step).
    """
    warmup_bytes = WARMUP_TIB * TIB
    ub_warmup_bytes = UB_AVG_WARMUP_TIB * TIB
    rows = []
    for f in glob.glob(os.path.join(dp_dir, "dp.*")):
        m = re.search(r"dp\.([\d.]+)$", f)
        if not m: continue
        name_UB = float(m.group(1))
        if name_UB < 0.20 or name_UB > 0.95: continue
        baseline = None
        last = None
        ub_sum = 0.0
        ub_cnt = 0
        with open(f) as fh:
            for line in fh:
                if "compacted_blocks:" not in line: continue
                wm = re.search(r"write_size_to_cache:\s*(\d+)", line)
                cm = re.search(r"(?<!ghost_)compacted_blocks:\s*(\d+)", line)
                em = re.search(r"evicted_blocks:\s*(\d+)", line)
                gm = re.search(r"global_valid_blocks:\s*(\d+)", line)
                tm = re.search(r"total_cache_size:\s*(\d+)", line)
                if not (wm and cm and em and gm and tm): continue
                w, c, e = int(wm.group(1)), int(cm.group(1)), int(em.group(1))
                g, tot = int(gm.group(1)), int(tm.group(1))
                if baseline is None and w >= warmup_bytes:
                    baseline = (w, c, e)
                last = (w, c, e)
                if tot > 0 and w >= ub_warmup_bytes:
                    ub_sum += g * PAGE / (tot + FREE_SEG_BYTES)
                    ub_cnt += 1
        if baseline is None or last is None or ub_cnt == 0: continue
        avg_UB = ub_sum / ub_cnt
        host  = (last[0] - baseline[0]) / TIB
        comp  = (last[1] - baseline[1]) * PAGE / TIB
        evict = (last[2] - baseline[2]) * PAGE / TIB
        tec   = host + comp + QLC_FACTOR * evict
        rows.append((avg_UB, host, comp, evict, tec))
    rows.sort(key=lambda x: x[0])
    return rows

def parse_reflash(stat_tag, gsdec_tag):
    """Return (avg_UB, host_TiB, comp_TiB, evict_TiB, raw_TEC) using the SAME definition as parse_dp_sweep:
       avg_UB = mean of g·PAGE/total_cache over rows where w >= 6 TiB,
       TEC    = (host + comp + 8.64·evict) delta from first w>=6TiB row to last row."""
    stat_path = f"/home/sejun000/ssd_waf/GS_FINAL_clean_{stat_tag}.stat_pr864"
    if not os.path.exists(stat_path): return None
    warmup_bytes = WARMUP_TIB * TIB
    ub_warmup_bytes = UB_AVG_WARMUP_TIB * TIB
    baseline, last = None, None
    ub_sum = 0.0; ub_cnt = 0
    with open(stat_path) as f:
        for line in f:
            if "LOG_GREEDY_COST_BENEFIT_10_GS_FINAL invalidate_blocks" not in line: continue
            wm = re.search(r"write_size_to_cache:\s*(\d+)", line)
            cm = re.search(r"(?<!ghost_)compacted_blocks:\s*(\d+)", line)
            em = re.search(r"evicted_blocks:\s*(\d+)", line)
            gm = re.search(r"global_valid_blocks:\s*(\d+)", line)
            tm = re.search(r"total_cache_size:\s*(\d+)", line)
            if not (wm and cm and em and gm and tm): continue
            w, c, e = int(wm.group(1)), int(cm.group(1)), int(em.group(1))
            g, tot = int(gm.group(1)), int(tm.group(1))
            if baseline is None and w >= warmup_bytes:
                baseline = (w, c, e)
            last = (w, c, e)
            if tot > 0 and w >= ub_warmup_bytes:
                ub_sum += g * PAGE / (tot + FREE_SEG_BYTES)
                ub_cnt += 1
    if baseline is None or last is None or ub_cnt == 0: return None
    avg_UB = ub_sum / ub_cnt
    host = (last[0] - baseline[0]) / TIB
    comp = (last[1] - baseline[1]) * PAGE / TIB
    ev   = (last[2] - baseline[2]) * PAGE / TIB
    tec  = host + comp + QLC_FACTOR * ev
    return avg_UB, host, comp, ev, tec

plt.rcParams.update({
    "font.size": 22,
    "axes.titlesize": 22,
    "axes.labelsize": 22,
    "xtick.labelsize": 18,
    "ytick.labelsize": 18,
    "legend.fontsize": 17,
})

fig, axes = plt.subplots(2, 2, figsize=(13, 9.6))
axes = axes.flatten()

SUBTAGS = ["(a)", "(b)", "(c)", "(d)"]
sweep_handle = refl_handle = None

for i, (label, dp_dir, stat_tag, gsdec_tag) in enumerate(TRACES):
    ax = axes[i]
    rows = parse_dp_sweep(dp_dir)
    if not rows:
        ax.set_title(f"{label}\n(no dp data)")
        continue
    max_tec = max(r[4] for r in rows)
    rows_plot = [r for r in rows if r[0] >= 0.55]
    UBs = [r[0] for r in rows_plot]
    nTECs = [r[4]/max_tec for r in rows_plot]

    h_sweep, = ax.plot(UBs, nTECs, color="black", linewidth=2.0, marker="o", markersize=4,
                       label=r"$U_B$ sweep")
    if sweep_handle is None: sweep_handle = h_sweep

    # REFLASH point computed from stat file using the same sweep definition.
    ref = parse_reflash(stat_tag, gsdec_tag)
    if ref is not None:
        ref_UB, _, _, _, ref_tec = ref
        ref_nTEC = ref_tec / max_tec
        h_refl, = ax.plot(ref_UB, ref_nTEC, marker="D", color="red",
                          markersize=14, zorder=6, markeredgecolor="black",
                          markeredgewidth=1.2, label="REFLASH")
        if refl_handle is None: refl_handle = h_refl

    ax.set_xlabel(r"$U_B$", labelpad=0)
    if i % 2 == 0:
        ax.set_ylabel("Normalized TEC")
    ax.set_ylim(0.5, 1.05)
    ax.set_xlim(0.50, 0.92)
    ax.grid(True, alpha=0.3)
    ax.set_title(f"{SUBTAGS[i]} {label}", y=-0.30)

handles = [h for h in (sweep_handle, refl_handle) if h is not None]
labels  = [r"$U_B$ sweep", "REFLASH"][:len(handles)]
fig.legend(handles, labels, loc="upper center", bbox_to_anchor=(0.5, 0.98),
           ncol=len(handles), frameon=False, fontsize=20)

fig.subplots_adjust(top=0.92, bottom=0.10, hspace=0.45)
fig.savefig("/home/sejun000/ssd_waf/AA_reflash_vs_sweet.pdf", bbox_inches="tight")
print("saved AA_reflash_vs_sweet.pdf")
