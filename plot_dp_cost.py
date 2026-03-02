import glob
import re
import matplotlib.pyplot as plt

# ── Configurable: skip first N TB of write_size_to_cache as warmup ──
WARMUP_TB = 4  # diff baseline: values at write_size_to_cache >= WARMUP_TB
WARMUP_BYTES = WARMUP_TB * (1024**4)

BLK_TO_TB = 4096 / (1024**4)

def parse_dp_files(directory, min_dp=0.55):
    dp_files = glob.glob(f"{directory}/dp.*")
    data = []
    for f in dp_files:
        m = re.search(r'dp\.([\d.]+)$', f)
        if not m:
            continue
        dp_num = float(m.group(1))
        if dp_num < min_dp:
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
    cost = [(d[1] * BLK_TO_TB) + 8.64* (d[2] * BLK_TO_TB) for d in data]
    return dp_nums, cost

greedy_dp, greedy_cost = parse_dp_files("/home/sejun000/ssd_waf/greedy_11_dwpd2")
cb_dp, cb_cost = parse_dp_files("/home/sejun000/ssd_waf/cb_11_dwpd2")

fig, ax = plt.subplots(figsize=(12, 6))
ax.scatter(greedy_dp, greedy_cost, color='tab:blue', zorder=3, label='Greedy')
ax.scatter(cb_dp, cb_cost, color='tab:red', zorder=3, label='Cost-Benefit')
ax.set_xlabel('Buffer Utilization')
ax.set_ylabel('Cost (TB)')
ax.set_ylim(bottom=0)
ax.legend()
ax.grid(True, alpha=0.3)
plt.title(f'Host-level GC + 8.64 * Flush writes  [warmup={WARMUP_TB}TB]')
plt.tight_layout()
plt.savefig('/home/sejun000/ssd_waf/dp_cost_plot.png', dpi=150)
print("Saved to dp_cost_plot.png")

print("\n=== Greedy ===")
for dp, c in zip(greedy_dp, greedy_cost):
    print(f"dp={dp:.2f}  cost={c:.2f} TB")

print("\n=== Cost-Benefit ===")
for dp, c in zip(cb_dp, cb_cost):
    print(f"dp={dp:.2f}  cost={c:.2f} TB")
