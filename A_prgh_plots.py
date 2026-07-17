#!/usr/bin/env python3
import re
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

PAGE = 4096
TIB = 2**40

plt.rcParams.update({
    "font.size": 27,        # 30 * 0.9
    "axes.titlesize": 27,
    "axes.labelsize": 27,
    "xtick.labelsize": 25,
    "ytick.labelsize": 25,
    "legend.fontsize": 25,
})

def parse(path):
    last = None
    with open(path) as f:
        for line in f:
            if "LOG_GREEDY_COST_BENEFIT_10_GS_FINAL invalidate_blocks" in line:
                last = line
    if not last: return None
    def gi(k):
        m = re.search(rf'(?<!ghost_){re.escape(k)}: (\d+)', last)
        return int(m.group(1)) if m else 0
    host = gi("write_size_to_cache")
    comp = gi("compacted_blocks")
    ev   = gi("evicted_blocks")
    return dict(
        host_TiB  = host / TIB,
        comp_TiB  = comp * PAGE / TIB,
        evict_TiB = ev * PAGE / TIB,
    )

BASE = "/home/sejun000/ssd_waf/LOG_GREEDY_COST_BENEFIT_10_GS_FINAL_us02_ewma_hl1572864_gsdec{RT}PrGhD{D}_d{D}.stat_pr{RT}"

D_list = [1, 2, 4, 8, 16]
r_list = [(2.88, "288"), (5.76, "576"), (8.64, "864"), (11.52, "1152"), (14.40, "1440")]

data_tec = {2.88: [], 8.64: []}
for r, RT in [(2.88, "288"), (8.64, "864")]:
    for D in D_list:
        d = parse(BASE.format(RT=RT, D=D))
        if d:
            tec = d["host_TiB"] + d["comp_TiB"] + r * d["evict_TiB"]
            data_tec[r].append((D, tec))

rs, hosts, comps, evicts = [], [], [], []
for r, RT in r_list:
    d = parse(BASE.format(RT=RT, D=1))
    if d:
        rs.append(r); hosts.append(d["host_TiB"]); comps.append(d["comp_TiB"]); evicts.append(d["evict_TiB"])

fig, axes = plt.subplots(1, 2, figsize=(14, 4.4), gridspec_kw={"wspace": 0.28, "width_ratios": [1, 1.4]})

# === Left: grouped bar — TEC normalized to D=1 per r ===
ax = axes[0]
import numpy as np
xpos_d = np.arange(len(D_list))
bar_w = 0.38
colors_r = {2.88: "#4c78a8", 8.64: "#f58518"}
for i, r in enumerate([2.88, 8.64]):
    pts = data_tec[r]
    tec_by_D = {D: tec for D, tec in pts}
    base = tec_by_D[1]
    ys = [tec_by_D[D] / base for D in D_list]
    offset = (i - 0.5) * bar_w
    ax.bar(xpos_d + offset, ys, bar_w, color=colors_r[r], label=f"r={r}")
ax.set_xticks(xpos_d)
ax.set_xticklabels(["REFLASH" if d == 1 else str(d) for d in D_list])
ax.set_xlabel("D")
ax.set_ylabel("TEC (norm. to D=1)")
ax.set_ylim(0, 1.1)
ax.axhline(1.0, color="gray", linewidth=0.8, alpha=0.5)
ax.grid(True, alpha=0.3, axis="y")
ax.set_xlim(-0.5, len(D_list) - 0.5)
ax.legend(loc="lower center", bbox_to_anchor=(0.5, 1.02), ncol=2, frameon=False,
          handletextpad=0.4, columnspacing=1.0, borderaxespad=0.1)

# === Right: stacked 100% bar at D=1 across r ===
ax = axes[1]
totals = [h + c + e for h, c, e in zip(hosts, comps, evicts)]
host_pct  = [100 * h / t for h, t in zip(hosts, totals)]
comp_pct  = [100 * c / t for c, t in zip(comps, totals)]
evict_pct = [100 * e / t for e, t in zip(evicts, totals)]

xpos = list(range(len(rs)))
ax.bar(xpos, host_pct,  label="Host write", color="#4c78a8")
ax.bar(xpos, comp_pct,  bottom=host_pct, label="GC", color="#f58518")
bottom2 = [a + b for a, b in zip(host_pct, comp_pct)]
ax.bar(xpos, evict_pct, bottom=bottom2, label="Flush", color="#e45756")

for i in range(len(rs)):
    if host_pct[i]  > 3: ax.text(i, host_pct[i]/2,             f"{host_pct[i]:.1f}%",  ha="center", va="center", color="white", fontsize=16, fontweight="bold")
    if comp_pct[i]  > 3: ax.text(i, host_pct[i]+comp_pct[i]/2, f"{comp_pct[i]:.1f}%",  ha="center", va="center", color="black", fontsize=16, fontweight="bold")
    if evict_pct[i] > 3: ax.text(i, bottom2[i]+evict_pct[i]/2, f"{evict_pct[i]:.1f}%", ha="center", va="center", color="white", fontsize=16, fontweight="bold")

ax.set_xticks(xpos)
ax.set_xticklabels([f"{r:.2f}" for r in rs])
ax.set_ylim(0, 100)
ax.set_xlim(-0.5, len(rs) - 0.5)
ax.set_xlabel("r")
ax.set_ylabel("%", labelpad=-4)
ax.tick_params(axis="y", pad=1)
ax.legend(loc="lower center", bbox_to_anchor=(0.5, 1.02), ncol=3, frameon=False,
          handletextpad=0.3, columnspacing=0.6, borderaxespad=0.1)
ax.grid(True, alpha=0.3, axis="y")

# Subplot titles close below each x-axis
axes[0].set_title("(a) TEC vs D", y=-0.38)
axes[1].set_title("(b) Cost distribution", y=-0.38)

fig.subplots_adjust(wspace=0.28)
fig.savefig("/home/sejun000/ssd_waf/A_prgh_combined.pdf", bbox_inches="tight", pad_inches=0.05)
fig.savefig("/home/sejun000/ssd_waf/A_prgh_combined.png", dpi=120, bbox_inches="tight", pad_inches=0.05)
print("saved A_prgh_combined.{pdf,png}")
