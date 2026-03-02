import pandas as pd
import matplotlib.pyplot as plt
import sys

# Three datasets: SepBIT (old), Greedy, REFlash
files = {
    'SepBIT': 'utilization_distribution.csv',
    'Greedy': 'utilization_distribution.20260302_053405.csv',
    'REFlash': 'utilization_distribution.20260302_050124.csv',
}

# Override from CLI if provided
if len(sys.argv) >= 4:
    files['SepBIT'] = sys.argv[1]
    files['Greedy'] = sys.argv[2]
    files['REFlash'] = sys.argv[3]

colors = {'SepBIT': 'tab:blue', 'Greedy': 'tab:orange', 'REFlash': 'tab:green'}

fig, ax = plt.subplots(figsize=(10, 5))
width = 1.8
offsets = {'SepBIT': -width, 'Greedy': 0, 'REFlash': width}

for label, fname in files.items():
    df = pd.read_csv(fname)
    centers = (df['util_low'] + df['util_high']) / 2 + offsets[label]
    ax.bar(centers, df['fraction'], width=width, alpha=0.7,
           color=colors[label], edgecolor='black', linewidth=0.3, label=label)

ax.set_xlabel('Segment Utilization (%)')
ax.set_ylabel('Fraction of Segments')
ax.set_title('Segment Utilization Distribution')
ax.set_xlim(0, 102)
ax.legend()
plt.tight_layout()
plt.savefig('utilization_distribution.png', dpi=150)
print(f"Saved utilization_distribution.png")
