import glob
import re
import matplotlib.pyplot as plt
import numpy as np

# ── Configurable: skip first N TB of write_size_to_cache as warmup ──
WARMUP_TB = 6  # diff baseline: values at write_size_to_cache >= WARMUP_TB
WARMUP_BYTES = WARMUP_TB * (1024**4)
HOST_WRITE_TB = 8  # constant host writes after warmup (TiB -> TB approx)
QLC_FACTOR = 8.64

dp_files = glob.glob("/home/sejun000/ssd_waf/greedy_11_dwpd1/dp.*")

data = []
for f in dp_files:
    m = re.search(r'dp\.([\d.]+)$', f)
    if not m:
        continue
    dp_num = float(m.group(1))
    if dp_num < 0.50:
        continue

    baseline = None
    last_line_vals = None
    with open(f) as fh:
        for line in fh:
            if 'compacted_blocks:' not in line or 'evicted_blocks:' not in line:
                continue
            wm = re.search(r'write_size_to_cache:\s*(\d+)', line)
            cm = re.search(r'compacted_blocks:\s*(\d+)', line)
            em = re.search(r'evicted_blocks:\s*(\d+)', line)
            if not (wm and cm and em):
                continue
            w = int(wm.group(1))
            c = int(cm.group(1))
            e = int(em.group(1))
            if baseline is None and w >= WARMUP_BYTES:
                baseline = (c, e)
            last_line_vals = (c, e)

    if last_line_vals is None or baseline is None:
        continue

    diff_c = last_line_vals[0] - baseline[0]
    diff_e = last_line_vals[1] - baseline[1]
    data.append((dp_num, diff_c, diff_e))

data.sort(key=lambda x: x[0])
dp_nums = [d[0] for d in data]
BLK_TO_TB = 4096 / (1000**4)
compacted_tb = [d[1] * BLK_TO_TB for d in data]
evicted_tb = [d[2] * BLK_TO_TB for d in data]
cost_tb = [QLC_FACTOR * ev + (comp + HOST_WRITE_TB) for comp, ev in zip(compacted_tb, evicted_tb)]

# Normalize TEC: max = 1.0
max_cost = max(cost_tb)
normalized_tec = [c / max_cost for c in cost_tb]

plt.rcParams.update({'font.size': 28})

fig, ax1 = plt.subplots(figsize=(12, 7))

# Single left y-axis for both GC writes and Flush writes (same scale)
ax1.plot(dp_nums, compacted_tb, color='tab:blue', label='Host-level GC', zorder=3, marker='o', markersize=10, linewidth=2)
ax1.plot(dp_nums, evicted_tb, color='tab:red', label='Flush', zorder=3, marker='s', markersize=10, linewidth=2)
ax1.set_xlabel(r'$U_B$')
ax1.set_ylabel('Writes (TB)')
ax1.tick_params(axis='y')
ax1.set_yticks(np.arange(0, 24, 4))
ax1.set_ylim(bottom=0, top=20)

# Right axis for Normalized TEC (black)
ax2 = ax1.twinx()
ax2.plot(dp_nums, normalized_tec, color='black', label='Normalized TEC', zorder=3, marker='^', markersize=10, linewidth=2.5, linestyle='--')
ax2.set_ylabel('Normalized TEC')
ax2.tick_params(axis='y')
ax2.set_yticks(np.arange(0.5, 1.1, 0.1))
ax2.set_ylim(bottom=0.5, top=1.0)

# Legend (collected after sweet spot is added)

# Sweet spot marker (no legend entry)
min_idx = normalized_tec.index(min(normalized_tec))
ax2.plot(dp_nums[min_idx], normalized_tec[min_idx], marker='*', color='gold',
         markersize=35, zorder=5, markeredgecolor='black', markeredgewidth=1.5)
ax2.annotate('Sweet spot',
             xy=(dp_nums[min_idx], normalized_tec[min_idx]),
             xytext=(dp_nums[min_idx] - 0.02, normalized_tec[min_idx] - 0.08),
             fontsize=24, ha='center', va='top',
             arrowprops=dict(arrowstyle='->', color='black', lw=1.5))

lines1, labels1 = ax1.get_legend_handles_labels()
lines2, labels2 = ax2.get_legend_handles_labels()
ax1.legend(lines1 + lines2, labels1 + labels2,
           loc='lower center', bbox_to_anchor=(0.5, 1.0), ncol=3,
           frameon=False, fontsize=26)

ax1.grid(True, alpha=0.3, color='black', linestyle='--')
ax1.set_axisbelow(True)

plt.tight_layout()
plt.savefig('/home/sejun000/ssd_waf/A_dp_writes_plot2.pdf', dpi=150)
print("Saved to A_dp_writes_plot2.pdf")

min_idx = normalized_tec.index(min(normalized_tec))
print(f"\nMax cost: {max_cost:.2f} TB-eq (UB={dp_nums[cost_tb.index(max_cost)]:.2f}, normalized=1.00)")
print(f"Min cost: {min(cost_tb):.2f} TB-eq (UB={dp_nums[min_idx]:.2f}, normalized={min(normalized_tec):.3f})")
print(f"Reduction: {(1 - min(normalized_tec))*100:.1f}%\n")

for d in data:
    comp = d[1] * BLK_TO_TB
    ev = d[2] * BLK_TO_TB
    c = QLC_FACTOR * ev + (comp + HOST_WRITE_TB)
    print(f"dp={d[0]:.2f}  compacted={comp:>8.2f}TB  evicted={ev:>8.2f}TB  cost={c:>8.2f}TB-eq  nTEC={c/max_cost:.3f}")
