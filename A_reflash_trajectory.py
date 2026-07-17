#!/usr/bin/env python3
# REFLASH U_B trajectory over time for each trace.
# X: host writes (TiB), Y: tgt_after (target valid rate = U_B).
# Overlay: sweet_UB (from cb_11 dp sweep) as horizontal dashed line.
import os, re
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

PAGE = 4096
TIB = 2**40
QLC = 8.64
SUBTAGS = ["(a)", "(b)", "(c)", "(d)"]

TRACES = [
    # label,     gsdec_tag,                    dp_dir for sweet UB
    ("Alibaba1", "gsdec864PrGhD1_dwpd3_d1",    "/home/sejun000/ssd_waf/cb_11_dwpd1"),
    ("Alibaba2", "gsdec864PrGhD1_d1",          "/home/sejun000/ssd_waf/cb_11_dwpd2"),
    ("Alibaba3", "gsdec864PrGhD1_dwpd1_d1",    "/home/sejun000/ssd_waf/cb_11_dwpd3"),
    ("YCSB-A",   "gsdec864PrGhD1_ssdtr_d1",    "/home/sejun000/ssd_waf/cb_11_ssdtrace"),
]

def sweet_UB(dp_dir, warmup_tib=6):
    import glob
    best = (None, float("inf"))
    warmup_bytes = warmup_tib * TIB
    for f in glob.glob(os.path.join(dp_dir, "dp.*")):
        m = re.search(r"dp\.([\d.]+)$", f)
        if not m: continue
        UB = float(m.group(1))
        if UB < 0.2 or UB > 0.95: continue
        base, last = None, None
        with open(f) as fh:
            for line in fh:
                if "compacted_blocks:" not in line: continue
                wm = re.search(r"write_size_to_cache:\s*(\d+)", line)
                cm = re.search(r"(?<!ghost_)compacted_blocks:\s*(\d+)", line)
                em = re.search(r"evicted_blocks:\s*(\d+)", line)
                if not (wm and cm and em): continue
                w, c, e = int(wm.group(1)), int(cm.group(1)), int(em.group(1))
                if base is None and w >= warmup_bytes: base = (w, c, e)
                last = (w, c, e)
        if base is None or last is None: continue
        host  = (last[0] - base[0]) / TIB
        comp  = (last[1] - base[1]) * PAGE / TIB
        evict = (last[2] - base[2]) * PAGE / TIB
        tec = host + comp + QLC * evict
        if tec < best[1]:
            best = (UB, tec)
    return best[0]

def parse_trajectory(gsdec_path):
    """Return [(host_TiB, U_B), ...] from gsdec.log."""
    pts = []
    with open(gsdec_path) as f:
        next(f)  # skip header
        for line in f:
            cols = line.split()
            if len(cols) < 14: continue
            ts = int(cols[0])
            tgt_after = float(cols[13])
            host_tib = ts * PAGE / TIB
            pts.append((host_tib, tgt_after))
    return pts

plt.rcParams.update({
    "font.size": 22, "axes.titlesize": 22, "axes.labelsize": 22,
    "xtick.labelsize": 18, "ytick.labelsize": 18, "legend.fontsize": 20,
})

fig, axes = plt.subplots(2, 2, figsize=(13, 9.6))
axes = axes.flatten()

traj_handle = sweet_handle = None
for i, (label, gsdec_tag, dp_dir) in enumerate(TRACES):
    ax = axes[i]
    gsdec_path = f"/home/sejun000/ssd_waf/LOG_GREEDY_COST_BENEFIT_10_GS_FINAL_us02_ewma_hl1572864_{gsdec_tag}_pr864.gsdec.log"
    if not os.path.exists(gsdec_path):
        ax.set_title(f"{SUBTAGS[i]} {label}\n(no gsdec)")
        continue
    pts = parse_trajectory(gsdec_path)
    if not pts:
        ax.set_title(f"{SUBTAGS[i]} {label}\n(empty)")
        continue
    xs = [p[0] for p in pts]
    ys = [p[1] for p in pts]
    h, = ax.plot(xs, ys, color="red", linewidth=1.5, label=r"REFLASH $U_B$")
    if traj_handle is None: traj_handle = h

    ax.set_xlim(0, 14)
    ax.set_ylim(0.0, 1.0)
    if i % 2 == 0:
        ax.set_ylabel(r"$U_B$")
    ax.set_xlabel("Host writes (TiB)", labelpad=0)
    ax.grid(True, alpha=0.3)
    ax.set_title(f"{SUBTAGS[i]} {label}", y=-0.42)


fig.subplots_adjust(top=0.96, bottom=0.10, hspace=0.45)
fig.savefig("/home/sejun000/ssd_waf/A_reflash_trajectory.pdf", bbox_inches="tight")
print("saved A_reflash_trajectory.pdf")
