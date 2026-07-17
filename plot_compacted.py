import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np

plt.rcParams.update({'font.size': 14})

policies = ['Read-Aware-Circular', 'Read-Aware', 'DOGI', 'MiDAS', 'SepBIT', 'Greedy']
workloads = ['FIO-Zipf0.99 7:3', 'Alibaba Read-Intensive']

wa = {
    'Read-Aware-Circular': [2.94, 2.01],
    'Read-Aware':          [2.75, 2.20],
    'DOGI':                [2.50, 1.89],
    'MiDAS':               [2.83, 1.94],
    'SepBIT':              [3.06, 2.46],
    'Greedy':              [3.50, 2.75],
}

x = np.arange(len(workloads))
n = len(policies)
width = 0.13
colors = ['#4472C4', '#ED7D31', '#70AD47', '#FFC000', '#9DC3E6', '#A5A5A5']

fig, ax = plt.subplots(1, 1, figsize=(10, 5.5))

for i, policy in enumerate(policies):
    offset = (i - (n-1)/2) * width
    bars = ax.bar(x + offset, wa[policy], width, label=policy, color=colors[i], edgecolor='black', linewidth=0.8)
    ax.bar_label(bars, fmt='%.2f', fontsize=9)

ax.set_ylabel('Write Amplification')
ax.set_xticks(x)
ax.set_xticklabels(workloads)

ax.legend(loc='upper center', ncol=3, fontsize=11, bbox_to_anchor=(0.5, 1.15), frameon=False)

plt.tight_layout(rect=[0, 0, 1, 0.90])
plt.savefig('plot_compacted.pdf', dpi=150, bbox_inches='tight')
plt.savefig('plot_compacted.png', dpi=150, bbox_inches='tight')
print('Saved plot_compacted.pdf and plot_compacted.png')
