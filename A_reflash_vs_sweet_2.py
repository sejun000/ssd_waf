#!/usr/bin/env python3
# Row-scatter version of A_reflash_vs_sweet:
#   x = U_B = global_valid_blocks * PAGE / total_cache_size
#   y = Δcompacted_blocks + 8.64·Δevicted_blocks  (per row, raw blocks)
# One point per stat row across all dp.0.* sweep files, plus REFLASH stat rows.
import glob, re, os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

PAGE = 4096
QLC = 8.64
TIB = 1099511627776

TRACES = [
    # label,        dp sweep dir,                                   REFLASH stat path
    ("Alibaba1",    "/home/sejun000/ssd_waf/cb_11_dwpd1",
     "/home/sejun000/ssd_waf/GS_FINAL_clean_dwpd2_5x.stat_pr864"),
    ("Alibaba2",    "/home/sejun000/ssd_waf/cb_11_dwpd2",
     "/home/sejun000/ssd_waf/GS_FINAL_clean_dwpd1to2.stat_pr864"),
    ("Alibaba3",    "/home/sejun000/ssd_waf/ali3_dp",
     "/home/sejun000/ssd_waf/GS_FINAL_clean_dwpd01to1.stat_pr864"),
    ("SSDtrace4x",  "/home/sejun000/ssd_waf/ssdtrace_dp",
     "/home/sejun000/ssd_waf/GS_FINAL_clean_scaled4x.stat_pr864"),
]

def parse_rows(path, line_match):
    """Iterate stat-style rows in `path`, yielding (U_B, dc, de) per row."""
    prev_c = prev_e = None
    out = []
    if not os.path.exists(path):
        return out
    with open(path) as fh:
        for line in fh:
            if line_match not in line:
                continue
            gm = re.search(r"global_valid_blocks:\s*(\d+)", line)
            tm = re.search(r"total_cache_size:\s*(\d+)", line)
            cm = re.search(r"(?<!ghost_)compacted_blocks:\s*(\d+)", line)
            em = re.search(r"evicted_blocks:\s*(\d+)", line)
            if not (gm and tm and cm and em):
                continue
            g = int(gm.group(1)); t = int(tm.group(1))
            c = int(cm.group(1)); e = int(em.group(1))
            if t <= 0:
                continue
            ub = g * PAGE / t
            if prev_c is None:
                prev_c, prev_e = c, e
                continue
            dc = c - prev_c
            de = e - prev_e
            prev_c, prev_e = c, e
            if dc < 0 or de < 0:
                continue
            out.append((ub, dc + QLC * de))
    return out

def sweep_points(dp_dir):
    xs, ys = [], []
    files = sorted(glob.glob(os.path.join(dp_dir, "dp.0.*")))
    for f in files:
        for ub, cost in parse_rows(f, "compacted_blocks:"):
            xs.append(ub); ys.append(cost)
    return xs, ys, len(files)

def reflash_points(stat_path):
    xs, ys = [], []
    for ub, cost in parse_rows(stat_path, "LOG_GREEDY_COST_BENEFIT_10_GS_FINAL invalidate_blocks"):
        xs.append(ub); ys.append(cost)
    return xs, ys

plt.rcParams.update({
    "font.size": 22, "axes.titlesize": 22, "axes.labelsize": 22,
    "xtick.labelsize": 18, "ytick.labelsize": 18, "legend.fontsize": 17,
})

fig, axes = plt.subplots(2, 2, figsize=(13, 9.6))
axes = axes.flatten()
SUBTAGS = ["(a)", "(b)", "(c)", "(d)"]

h_sweep = h_refl = None
for i, (label, dp_dir, stat_path) in enumerate(TRACES):
    ax = axes[i]
    sx, sy, nfiles = sweep_points(dp_dir)
    rx, ry = reflash_points(stat_path)

    if sx:
        s, = ax.plot(sx, sy, marker="o", linestyle="none",
                     markersize=2, alpha=0.25, color="tab:blue",
                     markeredgecolor="none", label=r"$U_B$ sweep rows")
        if h_sweep is None: h_sweep = s
    if rx:
        r, = ax.plot(rx, ry, marker="D", linestyle="none",
                     markersize=4, alpha=0.45, color="red",
                     markeredgecolor="none", label="REFLASH rows")
        if h_refl is None: h_refl = r

    ax.set_xlabel(r"$U_B$", labelpad=0)
    if i % 2 == 0:
        ax.set_ylabel(r"$\Delta$comp + 8.64$\cdot\Delta$evict (blk/row)")
    ax.set_xlim(0, 1.0)
    ax.grid(True, alpha=0.3, linestyle="--")
    ax.set_axisbelow(True)
    ax.set_title(f"{SUBTAGS[i]} {label}  (sweep files={nfiles}, "
                 f"sweep pts={len(sx)}, REFLASH pts={len(rx)})", y=-0.42, fontsize=14)

handles = [h for h in (h_sweep, h_refl) if h is not None]
labels  = [r"$U_B$ sweep rows", "REFLASH rows"][:len(handles)]
fig.legend(handles, labels, loc="upper center", bbox_to_anchor=(0.5, 0.98),
           ncol=len(handles), frameon=False, fontsize=20)

fig.subplots_adjust(top=0.92, bottom=0.10, hspace=0.55)
out = "/home/sejun000/ssd_waf/A_reflash_vs_sweet_2.pdf"
fig.savefig(out, bbox_inches="tight")
print(f"saved {out}")
