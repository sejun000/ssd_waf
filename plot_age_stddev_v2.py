import pandas as pd
import matplotlib.pyplot as plt
import numpy as np

BLK_TO_TB = 4096.0 / (1024**4)

# inv_time_scatter files (age stddev @ 10TB snapshot + inv_time stddev)
inv_files = {
    'SepBIT':    'LOG_SEPBIT_FIFO.inv_time_scatter.20260304_043508.csv',
    'Greedy':    'LOG_GREEDY_80.inv_time_scatter.20260304_043508.csv',
    'REFlash':   'LOG_GREEDY_COST_BENEFIT_80.inv_time_scatter.20260304_043508.csv',
    'REFlash-C': 'LOG_GREEDY_COST_BENEFIT_80_COLD.inv_time_scatter.20260304_041509.csv',
}

# segment_age_scatter for MIDAS (end-of-sim, no inv_time)
age_only_files = {
    'MiDAS': 'MIDAS_CACHE.segment_age_scatter.20260304_043508.csv',
}

colors = {
    'SepBIT': 'tab:blue', 'Greedy': 'tab:orange',
    'REFlash': 'tab:green', 'REFlash-C': 'tab:purple', 'MiDAS': 'tab:red',
}
labels_order = ['SepBIT', 'Greedy', 'REFlash', 'REFlash-C', 'MiDAS']

plt.rcParams.update({'font.size': 18})
fig, (ax_age, ax_inv, ax_bar) = plt.subplots(1, 3, figsize=(20, 6),
                                              gridspec_kw={'width_ratios': [2, 2, 1.5]})

# ── (a) Age Stddev CDF ──
for label in labels_order:
    if label in inv_files:
        df = pd.read_csv(inv_files[label])
        col = 'age_stddev'
    elif label in age_only_files:
        df = pd.read_csv(age_only_files[label])
        col = 'block_age_stddev'
    else:
        continue
    vals = np.sort(df[col].dropna().values * BLK_TO_TB)
    cdf = np.arange(1, len(vals) + 1) / len(vals)
    ax_age.plot(np.clip(vals, 0, 2.0), cdf, color=colors[label], linewidth=2.5)

ax_age.set_xlim(0, 2.0)
ax_age.set_xlabel('Block Age Stddev (TB)')
ax_age.set_ylabel('CDF')
ax_age.set_title('(a) Age Stddev')
ax_age.grid(True, alpha=0.3)

# ── (b) Left Lifetime (Inv Time) Stddev CDF ──
for label in labels_order:
    if label not in inv_files:
        continue
    df = pd.read_csv(inv_files[label])
    df = df[df['inv_count'] >= 10]  # filter low-count segments
    vals = np.sort(df['inv_time_stddev'].dropna().values * BLK_TO_TB)
    if len(vals) == 0:
        continue
    cdf = np.arange(1, len(vals) + 1) / len(vals)
    ax_inv.plot(np.clip(vals, 0, 2.0), cdf, color=colors[label], linewidth=2.5)

ax_inv.set_xlim(0, 2.0)
ax_inv.set_xlabel('Left Lifetime Stddev (TB)')
ax_inv.set_ylabel('CDF')
ax_inv.set_title('(b) Left Lifetime Stddev')
ax_inv.grid(True, alpha=0.3)

# ── (c) GC Writes bar chart ──
gc_tb = {}
stat_map = {
    'SepBIT':    'LOG_SEPBIT_FIFO.stat.log.20260304_043508',
    'Greedy':    'LOG_GREEDY_80.stat.log.20260304_043508',
    'REFlash':   'LOG_GREEDY_COST_BENEFIT_80.stat.log.20260304_043508',
    'REFlash-C': 'LOG_GREEDY_COST_BENEFIT_80_COLD.stat.log.20260304_041509',
    'MiDAS':     'MIDAS_CACHE.stat.log.20260304_043508',
}
for label, stat_file in stat_map.items():
    try:
        with open(stat_file) as f:
            lines = [l for l in f.readlines() if 'compacted_blocks:' in l]
        if lines:
            parts = lines[-1].split()
            for i, p in enumerate(parts):
                if p == 'compacted_blocks:':
                    gc_tb[label] = int(parts[i + 1]) * BLK_TO_TB
                    break
    except Exception as e:
        print(f"Warning: {label}: {e}")

bar_labels = [l for l in labels_order if l in gc_tb]
bar_vals = [gc_tb[l] for l in bar_labels]
bar_colors = [colors[l] for l in bar_labels]
bars = ax_bar.bar(bar_labels, bar_vals, color=bar_colors, width=0.6)
for bar, val in zip(bars, bar_vals):
    ax_bar.text(bar.get_x() + bar.get_width() / 2, bar.get_height() + 0.1,
                f'{val:.1f}', ha='center', va='bottom', fontsize=16)
ax_bar.set_ylim(0, max(bar_vals) * 1.15 if bar_vals else 1)
ax_bar.set_ylabel('GC Writes (TB)')
ax_bar.set_title('(c) GC Writes')
ax_bar.grid(True, axis='y', alpha=0.3)

# ── Legend ──
handles = [plt.Line2D([], [], color=colors[l], linewidth=2.5) for l in labels_order]
fig.legend(handles, labels_order, loc='upper center', ncol=len(labels_order),
           bbox_to_anchor=(0.5, 1.02), frameon=False, fontsize=18)

plt.tight_layout(rect=[0, 0, 1, 0.94])
plt.savefig('age_stddev_cdf_v2.png', dpi=150, bbox_inches='tight')
print("Saved age_stddev_cdf_v2.png")
