import csv
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

plt.rcParams.update({'font.size': 18})

TOTAL_CACHE_BLOCKS = 1883510931456 / 4096.0
WARMUP_TB = 6.0
WARMUP_MB = WARMUP_TB * 1024 * 1024
BASE = '/home/sejun000/ssd_waf'

def load_dp_path(path):
    steps, ratios = [], []
    with open(path) as f:
        for row in csv.reader(f):
            steps.append(int(row[0]))
            ratios.append(float(row[1]))
    tb = [s * 10 / 1024 for s in steps]
    return tb, ratios

def load_reflash(path):
    ref_tb, ref_util = [], []
    with open(path) as f:
        reader = csv.DictReader(f)
        for row in reader:
            host_mb = float(row['host_write_MB'])
            vb = int(row['valid_blocks'])
            if host_mb < WARMUP_MB or vb == 0:
                continue
            ref_tb.append((host_mb - WARMUP_MB) / (1024 * 1024))
            ref_util.append(vb / TOTAL_CACHE_BLOCKS)
    return ref_tb, ref_util

ratios = [2, 4, 6, 8, 10]
colors_dp = ['#1f77b4', '#2ca02c', '#ff7f0e', '#9467bd', '#17becf']

fig, ax = plt.subplots(figsize=(12, 5))

# CSAL
csal_csv = f'{BASE}/ftl0_20260225_082514.csv'
csal_tb, csal_util = load_reflash(csal_csv)
ax.plot(csal_tb, csal_util, color='#A880C0', linewidth=2.5, alpha=0.8, label='CSAL')

# REFlash (r=8.64 version)
ref_csv = f'{BASE}/LOG_GREEDY_COST_BENEFIT_10_20260226_033138.csv'
ref_tb, ref_util = load_reflash(ref_csv)
ax.plot(ref_tb, ref_util, color='tab:red', linewidth=2.5, alpha=0.8, label='REFlash')

# NearOpt paths for each r
for i, r in enumerate(ratios):
    dp_tb, dp_ratios = load_dp_path(f'{BASE}/dp_path_cb_11_dwpd2_qlc{r}.csv')
    ax.plot(dp_tb, dp_ratios, color=colors_dp[i], linewidth=2, alpha=0.8, label=f'NearOpt (r={r})')

ax.set_xlim(0, None)
ax.set_ylim(0.0, 1.0)
ax.set_yticks([0.0, 0.25, 0.5, 0.75, 1.0])
ax.axhline(y=0.75, color='gray', linestyle=':', linewidth=1, alpha=0.5)
ax.axhline(y=0.25, color='gray', linestyle=':', linewidth=1, alpha=0.5)
ax.grid(True, alpha=0.2, linestyle='--')
ax.set_xlabel('Host Write (TB)')
ax.set_ylabel('BUtil')
ax.legend(fontsize=12, loc='lower right', ncol=2)
ax.text(0.02, 0.95, 'Ali2', transform=ax.transAxes, fontsize=20, fontweight='bold', va='top')

plt.tight_layout()
plt.savefig(f'{BASE}/A_dp_path_ali2_periodic.pdf', bbox_inches='tight')
plt.savefig(f'{BASE}/A_dp_path_ali2_periodic.png', dpi=150, bbox_inches='tight')
print("Saved: A_dp_path_ali2_periodic.pdf / .png")
