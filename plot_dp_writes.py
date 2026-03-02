import glob
import re
import matplotlib.pyplot as plt

# ── Configurable: skip first N TB of write_size_to_cache as warmup ──
WARMUP_TB = 4  # diff baseline: values at write_size_to_cache >= WARMUP_TB
WARMUP_BYTES = WARMUP_TB * (1024**4)

dp_files = glob.glob("/home/sejun000/ssd_waf/greedy_11_dwpd2/dp.*")

data = []
for f in dp_files:
    m = re.search(r'dp\.([\d.]+)$', f)
    if not m:
        continue
    dp_num = float(m.group(1))
    if dp_num < 0.55:
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
BLK_TO_TB = 4096 / (1024**4)
compacted_tb = [d[1] * BLK_TO_TB for d in data]
evicted_tb = [d[2] * BLK_TO_TB for d in data]

fig, ax1 = plt.subplots(figsize=(12, 6))

ax1.scatter(dp_nums, compacted_tb, color='tab:blue', label='Host-level GC writes', zorder=3)
ax1.set_xlabel('Buffer Utilization')
ax1.set_ylabel('Host-level GC writes (TB)', color='tab:blue')
ax1.tick_params(axis='y', labelcolor='tab:blue')
ax1.set_ylim(bottom=0)

ax2 = ax1.twinx()
ax2.scatter(dp_nums, evicted_tb, color='tab:red', label='Flush writes', zorder=3)
ax2.set_ylabel('Flush writes (TB)', color='tab:red')
ax2.tick_params(axis='y', labelcolor='tab:red')
ax2.set_ylim(bottom=0)

lines1, labels1 = ax1.get_legend_handles_labels()
lines2, labels2 = ax2.get_legend_handles_labels()
ax1.legend(lines1 + lines2, labels1 + labels2, loc='upper left')

ax1.grid(True, alpha=0.3)
plt.title(f'TLC / QLC Writes vs Valid Ratio  [warmup={WARMUP_TB}TB]')
plt.tight_layout()
plt.savefig('/home/sejun000/ssd_waf/dp_writes_plot.png', dpi=150)
print("Saved to dp_writes_plot.png")

for d in data:
    print(f"dp={d[0]:.2f}  compacted(TLC)={d[1]:>15,}  evicted(QLC)={d[2]:>15,}")
