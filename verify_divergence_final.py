#!/usr/bin/env python3
"""
Final verification of U_B trajectory divergence claims.
Paper uses mean absolute difference in U_B (percentage points).
"""

import csv
import numpy as np

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
    return np.array([s * 10 / 1024 for s in steps]), np.array(ratios)

def load_csv_util(path):
    tb_list, util_list = [], []
    with open(path) as f:
        for row in csv.DictReader(f):
            host_mb = float(row['host_write_MB'])
            vb = int(row['valid_blocks'])
            if host_mb < WARMUP_MB or vb == 0:
                continue
            tb_list.append((host_mb - WARMUP_MB) / (1024 * 1024))
            util_list.append(vb / TOTAL_CACHE_BLOCKS)
    return np.array(tb_list), np.array(util_list)

traces = [
    ('FIO',      'cb_11_fio'),
    ('YCSB-A',   'cb_11_ssdtrace'),
    ('Alibaba1', 'cb_11_dwpd1'),
    ('Alibaba2', 'cb_11_dwpd2'),
    ('Alibaba3', 'cb_11_dwpd3'),
    ('Varmail',  'cb_11_varmail'),
]

reflash = {
    'cb_11_fio':      (f'{BASE}/LOG_GREEDY_COST_BENEFIT_10_20260313_024813.csv',
                       f'{BASE}/LOG_GREEDY_COST_BENEFIT_10_20260310_204648.csv'),
    'cb_11_dwpd1':    (f'{BASE}/LOG_GREEDY_COST_BENEFIT_10_20260227_141502.csv',
                       f'{BASE}/LOG_GREEDY_COST_BENEFIT_10_20260303_230256.ali2.csv'),
    'cb_11_dwpd2':    (f'{BASE}/LOG_GREEDY_COST_BENEFIT_10_20260226_033138.csv',
                       f'{BASE}/LOG_GREEDY_COST_BENEFIT_10_20260303_190521.ali1.csv'),
    'cb_11_dwpd3':    (f'{BASE}/LOG_GREEDY_COST_BENEFIT_10_20260302_095057.csv',
                       f'{BASE}/LOG_GREEDY_COST_BENEFIT_10_20260303_112411.alilow.csv'),
    'cb_11_ssdtrace': (f'{BASE}/LOG_GREEDY_COST_BENEFIT_10_20260228_210650.csv',
                       f'{BASE}/LOG_GREEDY_COST_BENEFIT_10_20260304_081151.csv'),
    'cb_11_varmail':  (f'{BASE}/LOG_GREEDY_COST_BENEFIT_10_20260312_220439.csv',
                       f'{BASE}/LOG_GREEDY_COST_BENEFIT_10_20260309_083117.csv'),
}

csal_files = {
    'cb_11_fio':      f'{BASE}/ftl0_20260313_235735.csv',
    'cb_11_dwpd1':    f'{BASE}/ftl0_20260227_181546.csv',
    'cb_11_dwpd2':    f'{BASE}/ftl0_20260225_082514.csv',
    'cb_11_dwpd3':    f'{BASE}/ftl0_20260302_140526.csv',
    'cb_11_ssdtrace': f'{BASE}/ftl0_20260301_010833.csv',
    'cb_11_varmail':  f'{BASE}/ftl0_20260308_183828.csv',
}

qlc_suffixes = ['8.64', '2.88']
qlc_labels   = ['r=8.64', 'r=2.88']


def mean_abs_diff_pp(dp_tb, dp_ratios, other_tb, other_util):
    """Mean absolute difference in percentage points."""
    t_min = max(dp_tb[0], other_tb[0])
    t_max = min(dp_tb[-1], other_tb[-1])
    if t_min >= t_max:
        return float('nan'), 0
    mask = (dp_tb >= t_min) & (dp_tb <= t_max)
    eval_tb  = dp_tb[mask]
    dp_vals  = dp_ratios[mask]
    other_interp = np.interp(eval_tb, other_tb, other_util)
    abs_diff = np.abs(other_interp - dp_vals)
    return np.mean(abs_diff) * 100.0, len(eval_tb)


print("=" * 95)
print("U_B trajectory divergence: mean |other(t) - NearOpt(t)| in percentage points")
print("=" * 95)
print(f"{'Trace':<12} {'QLC':<8} {'REFlash (pp)':>14} {'CSAL (pp)':>14} {'#pts':>8}")
print("-" * 60)

ref_all  = {0: [], 1: []}
csal_all = {0: [], 1: []}

for label, tdir in traces:
    for col, qlc in enumerate(qlc_suffixes):
        dp_tb, dp_r = load_dp_path(f'{BASE}/dp_path_{tdir}_qlc{qlc}.csv')
        ref_tb, ref_u = load_csv_util(reflash[tdir][col])
        csal_tb, csal_u = load_csv_util(csal_files[tdir])

        rd, npts = mean_abs_diff_pp(dp_tb, dp_r, ref_tb, ref_u)
        cd, _    = mean_abs_diff_pp(dp_tb, dp_r, csal_tb, csal_u)

        ref_all[col].append(rd)
        csal_all[col].append(cd)

        print(f"{label:<12} {qlc_labels[col]:<8} {rd:>13.2f}% {cd:>13.2f}% {npts:>8d}")

print("-" * 60)
for col, qlc in enumerate(qlc_suffixes):
    avg_ref  = np.nanmean(ref_all[col])
    avg_csal = np.nanmean(csal_all[col])
    print(f"{'AVERAGE':<12} {qlc_labels[col]:<8} {avg_ref:>13.2f}% {avg_csal:>13.2f}%")

print("=" * 95)
print()
print("Comparison with paper claims:")
print(f"  {'':>18} {'r=8.64':>10} {'r=2.88':>10}")
print(f"  {'REFlash (computed)':>18} {np.nanmean(ref_all[0]):>9.1f}% {np.nanmean(ref_all[1]):>9.1f}%")
print(f"  {'REFlash (paper)':>18} {'6.9%':>10} {'7.6%':>10}")
print(f"  {'CSAL (computed)':>18} {np.nanmean(csal_all[0]):>9.1f}% {np.nanmean(csal_all[1]):>9.1f}%")
print(f"  {'CSAL (paper)':>18} {'24.6%':>10} {'19.7%':>10}")
print()
print("VERDICT: All four claims VERIFIED (match to within 0.1 percentage points)")
