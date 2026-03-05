import csv
import matplotlib.pyplot as plt

plt.rcParams.update({'font.size': 22})

TOTAL_CACHE_BLOCKS = 1883510931456 / 4096.0
WARMUP_TB = 6.0
WARMUP_MB = WARMUP_TB * 1024 * 1024

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

BASE = '/home/sejun000/ssd_waf'

traces = [
    ('Alibaba1', 'cb_11_dwpd1'),
    ('Alibaba2', 'cb_11_dwpd2'),
    ('Alibaba3', 'cb_11_dwpd3'),
    ('YCSB-A', 'cb_11_ssdtrace'),
]

# REFlash CSVs: [QLC=2.88, QLC=8.64] for each trace
reflash = {
    'cb_11_dwpd1': (f'{BASE}/LOG_GREEDY_COST_BENEFIT_10_20260303_190521.ali1.csv',
                    f'{BASE}/LOG_GREEDY_COST_BENEFIT_10_20260226_033138.csv'),
    'cb_11_dwpd2': (f'{BASE}/LOG_GREEDY_COST_BENEFIT_10_20260303_230256.ali2.csv',
                    f'{BASE}/LOG_GREEDY_COST_BENEFIT_10_20260227_141502.csv'),
    'cb_11_dwpd3': (f'{BASE}/LOG_GREEDY_COST_BENEFIT_10_20260303_112411.alilow.csv',
                    f'{BASE}/LOG_GREEDY_COST_BENEFIT_10_20260302_095057.csv'),
    'cb_11_ssdtrace': (f'{BASE}/LOG_GREEDY_COST_BENEFIT_10_20260304_081151.csv',
                       f'{BASE}/LOG_GREEDY_COST_BENEFIT_10_20260228_210650.csv'),
}

qlc_labels = ['r=2.88', 'r=8.64']
qlc_suffixes = ['2.88', '8.64']

fig, axes = plt.subplots(4, 2, figsize=(16, 21))
plt.subplots_adjust(wspace=0.15)

# Find global xmax
xmax = 0
for label, tdir in traces:
    for qlc in qlc_suffixes:
        tb, _ = load_dp_path(f'{BASE}/dp_path_{tdir}_qlc{qlc}.csv')
        xmax = max(xmax, tb[-1])

for row, (label, tdir) in enumerate(traces):
    for col, qlc in enumerate(qlc_suffixes):
        ax = axes[row][col]
        dp_tb, dp_ratios = load_dp_path(f'{BASE}/dp_path_{tdir}_qlc{qlc}.csv')
        ref_csv = reflash[tdir][col]
        ref_tb, ref_util = load_reflash(ref_csv)

        l1, = ax.plot(dp_tb, dp_ratios, color='tab:blue', linewidth=2.5)
        l2, = ax.plot(ref_tb, ref_util, color='tab:red', linewidth=2.5, alpha=0.8)

        ax.set_xlim(0, xmax)
        ax.set_ylim(0.0, 1.0)
        ax.grid(True, alpha=0.3, color='black', linestyle='--')
        ax.set_axisbelow(True)

        sub_label = chr(ord('a') + row * 2 + col)
        ax.set_xlabel(f'Host Write (TB)\n({sub_label}) {label}')

        if col == 1:
            ax.set_yticklabels([])

        if row == 0:
            ax.set_title(qlc_labels[col], fontsize=26, pad=10)

        if row == 0 and col == 0:
            ax.legend([l1, l2], ['NearOpt', 'REFlash'], loc='lower center',
                      bbox_to_anchor=(1.0, 1.05), ncol=2, frameon=False, fontsize=24)

axes[0][0].annotate('BUtil', xy=(0, 1.02), xycoords=('axes fraction', 'axes fraction'),
                    xytext=(-5, 8), textcoords='offset points',
                    fontsize=24, ha='left', va='bottom')

plt.tight_layout()
plt.subplots_adjust(wspace=0.05)
plt.savefig(f'{BASE}/A_dp_path_plot.pdf', dpi=150)
print("Saved to A_dp_path_plot.pdf")
