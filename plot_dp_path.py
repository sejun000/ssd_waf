import csv
import matplotlib.pyplot as plt

plt.rcParams.update({'font.size': 22 * 1.3})

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
    ('FIO', 'cb_11_fio'),
    ('YCSB-A', 'cb_11_ssdtrace'),
    ('Alibaba1', 'cb_11_dwpd1'),
    ('Alibaba2', 'cb_11_dwpd2'),
    ('Alibaba3', 'cb_11_dwpd3'),
    ('Varmail', 'cb_11_varmail'),
]

# REFlash CSVs: [QLC=8.64, QLC=2.88] for each trace
reflash = {
    'cb_11_fio': (f'{BASE}/LOG_GREEDY_COST_BENEFIT_10_20260313_024813.csv',
                  f'{BASE}/LOG_GREEDY_COST_BENEFIT_10_20260310_204648.csv'),
    'cb_11_dwpd1': (f'{BASE}/LOG_GREEDY_COST_BENEFIT_10_20260227_141502.csv',
                    f'{BASE}/LOG_GREEDY_COST_BENEFIT_10_20260303_230256.ali2.csv'),
    'cb_11_dwpd2': (f'{BASE}/LOG_GREEDY_COST_BENEFIT_10_20260226_033138.csv',
                    f'{BASE}/LOG_GREEDY_COST_BENEFIT_10_20260303_190521.ali1.csv'),
    'cb_11_dwpd3': (f'{BASE}/LOG_GREEDY_COST_BENEFIT_10_20260302_095057.csv',
                    f'{BASE}/LOG_GREEDY_COST_BENEFIT_10_20260303_112411.alilow.csv'),
    'cb_11_ssdtrace': (f'{BASE}/LOG_GREEDY_COST_BENEFIT_10_20260228_210650.csv',
                       f'{BASE}/LOG_GREEDY_COST_BENEFIT_10_20260304_081151.csv'),
    'cb_11_varmail': (f'{BASE}/LOG_GREEDY_COST_BENEFIT_10_20260312_220439.csv',
                      f'{BASE}/LOG_GREEDY_COST_BENEFIT_10_20260309_083117.csv'),
}

# CSAL (FTL) CSVs: same file for both QLC columns
csal = {
    'cb_11_fio': f'{BASE}/ftl0_20260313_235735.csv',
    'cb_11_dwpd1': f'{BASE}/ftl0_20260227_181546.csv',
    'cb_11_dwpd2': f'{BASE}/ftl0_20260225_082514.csv',
    'cb_11_dwpd3': f'{BASE}/ftl0_20260302_140526.csv',
    'cb_11_ssdtrace': f'{BASE}/ftl0_20260301_010833.csv',
    'cb_11_varmail': f'{BASE}/ftl0_20260308_183828.csv',
}

qlc_labels = ['r=8.64', 'r=2.88']
qlc_suffixes = ['8.64', '2.88']

nrows = len(traces)
fig, axes = plt.subplots(nrows, 2, figsize=(18, nrows * 4))
plt.subplots_adjust(wspace=0.15, hspace=0.15)

# Find global xmax
xmax = 0
for label, tdir in traces:
    for qlc in qlc_suffixes:
        tb, _ = load_dp_path(f'{BASE}/dp_path_{tdir}_qlc{qlc}.csv')
        xmax = max(xmax, tb[-1])

for row, (label, tdir) in enumerate(traces):
    is_last_row = (row == nrows - 1)
    for col, qlc in enumerate(qlc_suffixes):
        ax = axes[row][col]
        dp_tb, dp_ratios = load_dp_path(f'{BASE}/dp_path_{tdir}_qlc{qlc}.csv')
        ref_entry = reflash.get(tdir)
        ref_csv = ref_entry[col] if ref_entry else None
        csal_csv = csal.get(tdir)

        # Plot order: CSAL, REFlash, NearOpt (later drawn on top)
        l3 = None
        if csal_csv is not None:
            csal_tb, csal_util = load_reflash(csal_csv)
            l3, = ax.plot(csal_tb, csal_util, color='#A880C0', linewidth=2.5, alpha=0.8)
        l2 = None
        if ref_csv is not None:
            ref_tb, ref_util = load_reflash(ref_csv)
            l2, = ax.plot(ref_tb, ref_util, color='tab:red', linewidth=2.5, alpha=0.8)
        l1, = ax.plot(dp_tb, dp_ratios, color='tab:blue', linewidth=2.5)

        ax.set_xlim(0, xmax)
        ax.set_ylim(0.0, 1.0)
        ax.set_yticks([0.0, 0.25, 0.5, 0.75, 1.0])
        ax.axhline(y=0.75, color='gray', linestyle=':', linewidth=1.5, alpha=0.6)
        ax.axhline(y=0.25, color='gray', linestyle=':', linewidth=1.5, alpha=0.6)
        ax.grid(True, alpha=0.3, color='black', linestyle='--')
        ax.set_axisbelow(True)

        # Only show xlabel on last row
        if is_last_row:
            sub_label = chr(ord('a') + col)
            ax.set_xlabel(f'Host Write (TB)\n({sub_label}) {qlc_labels[col]}', labelpad=12)
        else:
            ax.set_xticklabels([])

        # Trace name inside plot (bottom-left)
        ax.text(0.03, 0.08, label, transform=ax.transAxes,
                fontsize=26, fontweight='bold', va='bottom', ha='left')

        ax.tick_params(axis='y', labelsize=22)

        if col > 0:
            ax.set_yticklabels([])

        if row == 0 and col == 0:
            handles = ([l3] if l3 else []) + ([l2] if l2 else []) + [l1]
            labels = (['CSAL'] if l3 else []) + (['REFlash'] if l2 else []) + ['NearOpt']
            ax.legend(handles, labels, loc='lower center',
                      bbox_to_anchor=(1.0, 1.05), ncol=len(labels), frameon=False, fontsize=24 * 1.3)

axes[0][0].annotate(r'$U_B$', xy=(0, 1.08), xycoords=('axes fraction', 'axes fraction'),
                    xytext=(-5, 12), textcoords='offset points',
                    fontsize=24 * 1.3, ha='right', va='bottom')

plt.tight_layout()
plt.subplots_adjust(wspace=0.05, hspace=0.15, top=0.95)
plt.savefig(f'{BASE}/A_dp_path_plot.pdf', dpi=150)
plt.savefig(f'{BASE}/A_dp_path_plot.png', dpi=150)
print("Saved to A_dp_path_plot.pdf and .png")
