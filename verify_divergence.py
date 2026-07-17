#!/usr/bin/env python3
"""Verify U_B trajectory divergence claims from the paper."""

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
    tb = [s * 10 / 1024 for s in steps]
    return np.array(tb), np.array(ratios)

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
    return np.array(ref_tb), np.array(ref_util)

traces = [
    ('FIO', 'cb_11_fio'),
    ('YCSB-A', 'cb_11_ssdtrace'),
    ('Alibaba1', 'cb_11_dwpd1'),
    ('Alibaba2', 'cb_11_dwpd2'),
    ('Alibaba3', 'cb_11_dwpd3'),
    ('Varmail', 'cb_11_varmail'),
]

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

csal = {
    'cb_11_fio': f'{BASE}/ftl0_20260313_235735.csv',
    'cb_11_dwpd1': f'{BASE}/ftl0_20260227_181546.csv',
    'cb_11_dwpd2': f'{BASE}/ftl0_20260225_082514.csv',
    'cb_11_dwpd3': f'{BASE}/ftl0_20260302_140526.csv',
    'cb_11_ssdtrace': f'{BASE}/ftl0_20260301_010833.csv',
    'cb_11_varmail': f'{BASE}/ftl0_20260308_183828.csv',
}

qlc_suffixes = ['8.64', '2.88']
qlc_labels = ['r=8.64', 'r=2.88']


def compute_divergence(dp_tb, dp_ratios, other_tb, other_util):
    """
    Interpolate the 'other' trajectory onto dp_path time points,
    then compute mean absolute relative difference.
    Only compare where both have coverage.
    """
    # Determine overlapping range
    t_min = max(dp_tb[0], other_tb[0])
    t_max = min(dp_tb[-1], other_tb[-1])

    if t_min >= t_max:
        return float('nan'), 0

    # Use dp_path time points within the overlap
    mask = (dp_tb >= t_min) & (dp_tb <= t_max)
    eval_tb = dp_tb[mask]
    dp_vals = dp_ratios[mask]

    if len(eval_tb) == 0:
        return float('nan'), 0

    # Interpolate other onto these time points
    other_interp = np.interp(eval_tb, other_tb, other_util)

    # Mean absolute difference (as percentage of NearOpt U_B)
    # Avoid division by zero
    nonzero = dp_vals > 0.01
    abs_diff = np.abs(other_interp[nonzero] - dp_vals[nonzero])
    rel_diff = abs_diff / dp_vals[nonzero]

    mean_rel_pct = np.mean(rel_diff) * 100.0
    return mean_rel_pct, len(eval_tb)


print("=" * 90)
print(f"{'Trace':<12} {'QLC':<8} {'REFlash div%':>14} {'CSAL div%':>14} {'#pts(ref)':>10} {'#pts(csal)':>10}")
print("=" * 90)

# Accumulate for averages
ref_divs = {0: [], 1: []}
csal_divs = {0: [], 1: []}

for label, tdir in traces:
    for col, qlc in enumerate(qlc_suffixes):
        dp_path_file = f'{BASE}/dp_path_{tdir}_qlc{qlc}.csv'
        dp_tb, dp_ratios = load_dp_path(dp_path_file)

        # REFlash
        ref_csv = reflash[tdir][col]
        ref_tb, ref_util = load_reflash(ref_csv)
        ref_div, ref_npts = compute_divergence(dp_tb, dp_ratios, ref_tb, ref_util)
        ref_divs[col].append(ref_div)

        # CSAL (same file for both columns)
        csal_csv = csal[tdir]
        csal_tb, csal_util = load_reflash(csal_csv)
        csal_div, csal_npts = compute_divergence(dp_tb, dp_ratios, csal_tb, csal_util)
        csal_divs[col].append(csal_div)

        print(f"{label:<12} {qlc_labels[col]:<8} {ref_div:>13.2f}% {csal_div:>13.2f}% {ref_npts:>10d} {csal_npts:>10d}")

print("=" * 90)

# Averages
for col, qlc in enumerate(qlc_suffixes):
    avg_ref = np.nanmean(ref_divs[col])
    avg_csal = np.nanmean(csal_divs[col])
    print(f"{'AVERAGE':<12} {qlc_labels[col]:<8} {avg_ref:>13.2f}% {avg_csal:>13.2f}%")

print("=" * 90)
print()
print("Paper claims:")
print("  REFlash:  6.9% at r=8.64,  7.6% at r=2.88")
print("  CSAL:    24.6% at r=8.64, 19.7% at r=2.88")
