#!/usr/bin/env python3
import csv
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

csv_path = 'mrc_8tb_alibaba_dwpd01to1.csv'
out_path = 'mrc_alibaba_dwpd01to1.png'

cache_gb = []
miss_rate = []

with open(csv_path) as f:
    reader = csv.DictReader(f)
    for row in reader:
        blocks = int(row['CacheSize(blocks)'])
        mr = float(row['MissRate(%)'])
        gb = blocks * 4096 / (1024**3)
        cache_gb.append(gb)
        miss_rate.append(mr)

fig, ax = plt.subplots(figsize=(10, 6))
ax.plot(cache_gb, miss_rate, linewidth=1.5, color='#2196F3')
ax.set_xlabel('Cache Size (GB)', fontsize=13)
ax.set_ylabel('Miss Rate (%)', fontsize=13)
ax.set_title('MRC — Alibaba DWPD 0.1-to-1 (8TB written, LRU)', fontsize=14)
ax.set_xlim(left=0)
ax.set_ylim(bottom=0)
ax.grid(True, alpha=0.3)
fig.tight_layout()
fig.savefig(out_path, dpi=150)
print(f'Saved to {out_path}')
