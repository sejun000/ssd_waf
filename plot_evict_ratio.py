import os
import re
import matplotlib.pyplot as plt
import numpy as np

dp_values = []
evict_ratios = []

# dp 0.2 ~ 0.88
for i in range(20, 89):
    dp_val = i / 100.0
    fname = f"dp.0.{i}" if i >= 10 else f"dp.0.{i}"
    # handle dp.0.2 vs dp.0.20
    # check which file exists
    fpath = os.path.join("/home/sejun000/ssd_waf", f"dp.0.{i}")
    if not os.path.exists(fpath):
        # try single digit for .2, .3, etc
        if i % 10 == 0:
            fpath2 = os.path.join("/home/sejun000/ssd_waf", f"dp.0.{i // 10}")
            if os.path.exists(fpath2):
                fpath = fpath2
            else:
                print(f"Missing: dp {dp_val}")
                continue
        else:
            print(f"Missing: dp {dp_val}")
            continue

    # read last line that contains write_size_to_cache
    with open(fpath, 'r') as f:
        lines = f.readlines()
        if not lines:
            print(f"Empty: {fpath}")
            continue
        last_line = None
        for line in reversed(lines):
            if 'write_size_to_cache' in line:
                last_line = line.strip()
                break
        if last_line is None:
            print(f"No data: {fpath}")
            continue

    # extract write_size_to_cache and evicted_blocks
    m_write = re.search(r'write_size_to_cache:\s+(\d+)', last_line)
    m_evict = re.search(r'evicted_blocks:\s+(\d+)', last_line)

    if m_write and m_evict:
        host_written_bytes = int(m_write.group(1))
        evict_blocks = int(m_evict.group(1))
        # convert host_written to 4K blocks for fair comparison
        host_written_4k = host_written_bytes / 4096
        if host_written_4k > 0:
            evict_ratio = evict_blocks / host_written_4k
        else:
            evict_ratio = 0
        dp_values.append(dp_val)
        evict_ratios.append(evict_ratio)
        print(f"dp={dp_val:.2f}  host_written={host_written_bytes}  evict_blocks={evict_blocks}  ratio={evict_ratio:.4f}")
    else:
        print(f"Parse error: {fpath}")

# Sort by dp value
pairs = sorted(zip(dp_values, evict_ratios))
dp_values = [p[0] for p in pairs]
evict_ratios = [p[1] for p in pairs]

plt.figure(figsize=(12, 6))
plt.plot(dp_values, evict_ratios, 'b-o', markersize=4)
plt.xlabel('DP Value', fontsize=14)
plt.ylabel('Evict Ratio (evict_blocks / host_written_4k)', fontsize=14)
plt.title('Evict Ratio vs DP Value (0.2 ~ 0.88)', fontsize=16)
plt.grid(True, alpha=0.3)
plt.xticks(np.arange(0.2, 0.89, 0.05))
plt.tight_layout()
plt.savefig('/home/sejun000/ssd_waf/evict_ratio_plot.png', dpi=150)
plt.close()
print("\nPlot saved to evict_ratio_plot.png")
