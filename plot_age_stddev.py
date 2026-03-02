import pandas as pd
import matplotlib.pyplot as plt
import sys

files = {
    'SepBIT': 'segment_age_scatter.csv',
    'Greedy': 'segment_age_scatter.20260302_053405.csv',
    'REFlash': 'segment_age_scatter.20260302_050124.csv',
}

if len(sys.argv) >= 4:
    files['SepBIT'] = sys.argv[1]
    files['Greedy'] = sys.argv[2]
    files['REFlash'] = sys.argv[3]

colors = {'SepBIT': 'tab:blue', 'Greedy': 'tab:orange', 'REFlash': 'tab:green'}
BLK_TO_TB = 4096.0 / (1024**4)

import numpy as np

# CDF plot
fig, ax = plt.subplots(figsize=(8, 5))

for label, fname in files.items():
    df = pd.read_csv(fname)
    df['block_age_stddev_tb'] = df['block_age_stddev'] * BLK_TO_TB
    vals = np.sort(df['block_age_stddev_tb'].values)
    cdf = np.arange(1, len(vals) + 1) / len(vals)
    # clip to 2TB, rest shows as 1.0
    vals = np.clip(vals, 0, 2.0)
    ax.plot(vals, cdf, color=colors[label], linewidth=2, label=label)

ax.set_xlim(0, 2.0)
ax.set_xlabel('Block Age Stddev (TB)')
ax.set_ylabel('CDF')
ax.set_title('CDF of Per-Segment Block Age Standard Deviation')
ax.legend()
ax.grid(True, alpha=0.3)
plt.tight_layout()
plt.savefig('age_stddev_cdf.png', dpi=150)
print(f"Saved age_stddev_cdf.png")
