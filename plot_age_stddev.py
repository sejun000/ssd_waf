import pandas as pd
import matplotlib.pyplot as plt
import numpy as np
import re

inv_files = {
    'Greedy': 'LOG_GREEDY_80.inv_time_scatter.20260304_131034.csv',
    'SepBIT': 'LOG_SEPBIT_FIFO.inv_time_scatter.20260304_124656.csv',
    'REFlash': 'LOG_GREEDY_COST_BENEFIT_80.inv_time_scatter.20260304_111119.csv',
    'REFlash-CB': 'LOG_GREEDY_COST_BENEFIT_COLD_80.inv_time_scatter.20260304_145254.csv',
    'MiDAS': 'MIDAS_CACHE.inv_time_scatter.20260304_140327.csv',
}

stat_files = {
    'Greedy': 'LOG_GREEDY_80.stat.log.20260304_131034',
    'SepBIT': 'LOG_SEPBIT_FIFO.stat.log.20260304_124656',
    'REFlash': 'LOG_GREEDY_COST_BENEFIT_80.stat.log.20260304_111119',
    'REFlash-CB': 'LOG_GREEDY_COST_BENEFIT_COLD_80.stat.log.20260304_145254',
    'MiDAS': 'MIDAS_CACHE.stat.log.20260304_140327',
}

# Separate config for GC-rewritten lifetime panel
rewritten_stat_files = {
    'Greedy':     'LOG_GREEDY_80.stat.log.20260304_131034',
    'REFlash-CB': 'LOG_GREEDY_COST_BENEFIT_COLD_80.stat.log.20260304_145254',
    'REFlash':    'LOG_GREEDY_COST_BENEFIT_80.stat.log.20260304_111119',
}

colors = {'Greedy': '#FFB07C', 'SepBIT': '#87CEAB', 'MiDAS': '#C4A8D8', 'REFlash-CB': 'tab:blue', 'REFlash': 'tab:red'}
rewritten_colors = {'Greedy': '#FFB07C', 'REFlash-CB': 'tab:blue', 'REFlash': 'tab:red'}
BLK_TO_TB = 4096.0 / (1000**4)

gc_writes_blks = {
    'Greedy': 2386322142,
    'SepBIT': 4401616050,
    'REFlash': 1578750240,
    'REFlash-CB': 2303204956,
    'MiDAS': 2046290451,
}


# Histogram granularity (blocks per bucket)
GRANULARITY = 38535168

def parse_compacted_lifetime(stat_file):
    """Parse compacted_lifetime histogram from stat log."""
    buckets = {}
    in_section = False
    with open(stat_file) as f:
        for line in f:
            if '---summary of compacted_lifetime---' in line:
                in_section = True
                continue
            if in_section:
                if line.startswith('---'):
                    break
                m = re.match(r'(\d+)\s+th\s+(\d+)', line.strip())
                if m:
                    buckets[int(m.group(1))] = int(m.group(2))
    return buckets

plt.rcParams.update({'font.size': 20})

labels_order = ['Greedy', 'SepBIT', 'MiDAS', 'REFlash-CB', 'REFlash']
rewritten_order = list(rewritten_stat_files.keys())

fig, (ax_inv, ax_hist, ax_bar) = plt.subplots(1, 3, figsize=(21, 5),
                                               gridspec_kw={'width_ratios': [2, 2, 1.5]})

# --- Left: Remaining Lifetime Stddev CDF ---
for label in labels_order:
    fname = inv_files[label]
    df = pd.read_csv(fname)
    df['inv_time_stddev_tb'] = df['inv_time_stddev'] * BLK_TO_TB
    vals = np.sort(df['inv_time_stddev_tb'].values)
    cdf = np.arange(1, len(vals) + 1) / len(vals)
    vals = np.clip(vals, 0, 2.0)
    ax_inv.plot(vals, cdf, color=colors[label], linewidth=2.5, label=label)

ax_inv.set_xlim(0, 2.0)
ax_inv.set_ylim(0, 1.0)
ax_inv.set_xlabel('Remaining Lifetime Stddev (TB)')
ax_inv.set_ylabel('CDF')
ax_inv.legend(fontsize=16, loc='lower right')
ax_inv.grid(True, alpha=0.3)

# --- Middle: GC-Rewritten Lifetime (Greedy, Cold, Warm only) ---
tb_per_bucket = GRANULARITY * BLK_TO_TB
max_bucket = 20  # show up to ~2.9 TB

for label in rewritten_order:
    buckets = parse_compacted_lifetime(rewritten_stat_files[label])
    if not buckets:
        continue
    indices = sorted(buckets.keys())
    indices = [i for i in indices if i <= max_bucket]
    x_tb = [(i + 0.5) * tb_per_bucket for i in indices]
    counts = [buckets[i] for i in indices]
    total = sum(buckets.values())
    fracs = [c / total for c in counts]
    ax_hist.plot(x_tb, fracs, color=rewritten_colors[label], linewidth=2.5, label=label)

ax_hist.set_xlim(0, max_bucket * tb_per_bucket)
ax_hist.set_xlabel('GC-Rewritten Block Lifetime (TB)')
ax_hist.set_ylabel('Fraction')
ax_hist.legend(fontsize=16)
ax_hist.grid(True, alpha=0.3)

# --- Right: GC writes bar chart ---
bar_order = ['Greedy', 'SepBIT', 'MiDAS', 'REFlash-CB', 'REFlash']
gc_tb_vals = [gc_writes_blks[l] * BLK_TO_TB for l in bar_order]
bar_colors = [colors[l] for l in bar_order]
bars = ax_bar.bar(bar_order, gc_tb_vals, color=bar_colors, width=0.6)
ax_bar.set_ylabel('GC Writes (TB)')
for bar, val in zip(bars, gc_tb_vals):
    ax_bar.text(bar.get_x() + bar.get_width() / 2, bar.get_height() + 0.2,
                f'{val:.1f}', ha='center', va='bottom', fontsize=18)
ax_bar.set_ylim(0, max(gc_tb_vals) * 1.15)
ax_bar.set_xlabel('')
ax_bar.tick_params(axis='x', rotation=30)
ax_bar.grid(True, axis='y', alpha=0.3)

plt.tight_layout()

for ax, subtitle in [(ax_inv, '(a) Remaining lifetime stddev per segment'),
                      (ax_hist, '(b) GC-rewritten block lifetime distribution'),
                      (ax_bar, '(c) GC writes')]:
    ax.text(0.5, -0.35, subtitle, transform=ax.transAxes,
            ha='center', va='top', fontsize=20)

plt.savefig('A_age_stddev_cdf.pdf', dpi=150, bbox_inches='tight')
print("Saved A_age_stddev_cdf.pdf")
