#!/usr/bin/env python3
"""Verify U_B trajectory divergence claims - explore different methodologies."""

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
    tb = np.array([s * 10 / 1024 for s in steps])
    return tb, np.array(ratios)

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

csal_files = {
    'cb_11_fio': f'{BASE}/ftl0_20260313_235735.csv',
    'cb_11_dwpd1': f'{BASE}/ftl0_20260227_181546.csv',
    'cb_11_dwpd2': f'{BASE}/ftl0_20260225_082514.csv',
    'cb_11_dwpd3': f'{BASE}/ftl0_20260302_140526.csv',
    'cb_11_ssdtrace': f'{BASE}/ftl0_20260301_010833.csv',
    'cb_11_varmail': f'{BASE}/ftl0_20260308_183828.csv',
}

qlc_suffixes = ['8.64', '2.88']
qlc_labels = ['r=8.64', 'r=2.88']


def compute_divergence_abs(dp_tb, dp_ratios, other_tb, other_util, t_start=0.0):
    """Mean absolute difference (not relative)."""
    t_min = max(dp_tb[0], other_tb[0], t_start)
    t_max = min(dp_tb[-1], other_tb[-1])
    if t_min >= t_max:
        return float('nan'), 0
    mask = (dp_tb >= t_min) & (dp_tb <= t_max)
    eval_tb = dp_tb[mask]
    dp_vals = dp_ratios[mask]
    if len(eval_tb) == 0:
        return float('nan'), 0
    other_interp = np.interp(eval_tb, other_tb, other_util)
    abs_diff = np.abs(other_interp - dp_vals)
    return np.mean(abs_diff) * 100.0, len(eval_tb)  # as percentage points of U_B


def compute_divergence_rel(dp_tb, dp_ratios, other_tb, other_util, t_start=0.0):
    """Mean absolute relative difference (% of NearOpt)."""
    t_min = max(dp_tb[0], other_tb[0], t_start)
    t_max = min(dp_tb[-1], other_tb[-1])
    if t_min >= t_max:
        return float('nan'), 0
    mask = (dp_tb >= t_min) & (dp_tb <= t_max)
    eval_tb = dp_tb[mask]
    dp_vals = dp_ratios[mask]
    if len(eval_tb) == 0:
        return float('nan'), 0
    other_interp = np.interp(eval_tb, other_tb, other_util)
    nonzero = dp_vals > 0.01
    rel_diff = np.abs(other_interp[nonzero] - dp_vals[nonzero]) / dp_vals[nonzero]
    return np.mean(rel_diff) * 100.0, np.sum(nonzero)


def compute_divergence_median_rel(dp_tb, dp_ratios, other_tb, other_util, t_start=0.0):
    """Median absolute relative difference."""
    t_min = max(dp_tb[0], other_tb[0], t_start)
    t_max = min(dp_tb[-1], other_tb[-1])
    if t_min >= t_max:
        return float('nan'), 0
    mask = (dp_tb >= t_min) & (dp_tb <= t_max)
    eval_tb = dp_tb[mask]
    dp_vals = dp_ratios[mask]
    if len(eval_tb) == 0:
        return float('nan'), 0
    other_interp = np.interp(eval_tb, other_tb, other_util)
    nonzero = dp_vals > 0.01
    rel_diff = np.abs(other_interp[nonzero] - dp_vals[nonzero]) / dp_vals[nonzero]
    return np.median(rel_diff) * 100.0, np.sum(nonzero)


# =========================================================================
# Method 1: Mean absolute difference (percentage points)
# =========================================================================
print("=" * 100)
print("METHOD 1: Mean absolute difference in U_B (percentage points)")
print("=" * 100)
print(f"{'Trace':<12} {'QLC':<8} {'REFlash':>12} {'CSAL':>12}")
print("-" * 50)
ref_d = {0: [], 1: []}
csal_d = {0: [], 1: []}
for label, tdir in traces:
    for col, qlc in enumerate(qlc_suffixes):
        dp_tb, dp_ratios = load_dp_path(f'{BASE}/dp_path_{tdir}_qlc{qlc}.csv')
        ref_tb, ref_util = load_reflash(reflash[tdir][col])
        csal_tb, csal_util = load_reflash(csal_files[tdir])
        rd, _ = compute_divergence_abs(dp_tb, dp_ratios, ref_tb, ref_util)
        cd, _ = compute_divergence_abs(dp_tb, dp_ratios, csal_tb, csal_util)
        ref_d[col].append(rd)
        csal_d[col].append(cd)
        print(f"{label:<12} {qlc_labels[col]:<8} {rd:>11.2f}% {cd:>11.2f}%")
print("-" * 50)
for col, qlc in enumerate(qlc_suffixes):
    print(f"{'AVERAGE':<12} {qlc_labels[col]:<8} {np.nanmean(ref_d[col]):>11.2f}% {np.nanmean(csal_d[col]):>11.2f}%")


# =========================================================================
# Method 2: Mean relative difference (% of NearOpt), skip first 0.5 TB
# =========================================================================
for skip_tb in [0.0, 0.5, 1.0, 2.0]:
    print()
    print("=" * 100)
    print(f"METHOD 2: Mean relative diff (% of NearOpt), skip first {skip_tb} TB after warmup")
    print("=" * 100)
    print(f"{'Trace':<12} {'QLC':<8} {'REFlash':>12} {'CSAL':>12}")
    print("-" * 50)
    ref_d = {0: [], 1: []}
    csal_d = {0: [], 1: []}
    for label, tdir in traces:
        for col, qlc in enumerate(qlc_suffixes):
            dp_tb, dp_ratios = load_dp_path(f'{BASE}/dp_path_{tdir}_qlc{qlc}.csv')
            ref_tb, ref_util = load_reflash(reflash[tdir][col])
            csal_tb, csal_util = load_reflash(csal_files[tdir])
            rd, _ = compute_divergence_rel(dp_tb, dp_ratios, ref_tb, ref_util, t_start=skip_tb)
            cd, _ = compute_divergence_rel(dp_tb, dp_ratios, csal_tb, csal_util, t_start=skip_tb)
            ref_d[col].append(rd)
            csal_d[col].append(cd)
            print(f"{label:<12} {qlc_labels[col]:<8} {rd:>11.2f}% {cd:>11.2f}%")
    print("-" * 50)
    for col, qlc in enumerate(qlc_suffixes):
        print(f"{'AVERAGE':<12} {qlc_labels[col]:<8} {np.nanmean(ref_d[col]):>11.2f}% {np.nanmean(csal_d[col]):>11.2f}%")


# =========================================================================
# Method 3: Median relative difference
# =========================================================================
print()
print("=" * 100)
print("METHOD 3: Median relative diff (% of NearOpt)")
print("=" * 100)
print(f"{'Trace':<12} {'QLC':<8} {'REFlash':>12} {'CSAL':>12}")
print("-" * 50)
ref_d = {0: [], 1: []}
csal_d = {0: [], 1: []}
for label, tdir in traces:
    for col, qlc in enumerate(qlc_suffixes):
        dp_tb, dp_ratios = load_dp_path(f'{BASE}/dp_path_{tdir}_qlc{qlc}.csv')
        ref_tb, ref_util = load_reflash(reflash[tdir][col])
        csal_tb, csal_util = load_reflash(csal_files[tdir])
        rd, _ = compute_divergence_median_rel(dp_tb, dp_ratios, ref_tb, ref_util)
        cd, _ = compute_divergence_median_rel(dp_tb, dp_ratios, csal_tb, csal_util)
        ref_d[col].append(rd)
        csal_d[col].append(cd)
        print(f"{label:<12} {qlc_labels[col]:<8} {rd:>11.2f}% {cd:>11.2f}%")
print("-" * 50)
for col, qlc in enumerate(qlc_suffixes):
    print(f"{'AVERAGE':<12} {qlc_labels[col]:<8} {np.nanmean(ref_d[col]):>11.2f}% {np.nanmean(csal_d[col]):>11.2f}%")


# =========================================================================
# Method 4: Exclude FIO and Alibaba1 (the outliers), mean relative
# =========================================================================
print()
print("=" * 100)
print("METHOD 4: Mean relative diff, EXCLUDING FIO and Alibaba1 (4 traces)")
print("=" * 100)
print(f"{'Trace':<12} {'QLC':<8} {'REFlash':>12} {'CSAL':>12}")
print("-" * 50)
ref_d = {0: [], 1: []}
csal_d = {0: [], 1: []}
for label, tdir in traces:
    if tdir in ('cb_11_fio', 'cb_11_dwpd1'):
        continue
    for col, qlc in enumerate(qlc_suffixes):
        dp_tb, dp_ratios = load_dp_path(f'{BASE}/dp_path_{tdir}_qlc{qlc}.csv')
        ref_tb, ref_util = load_reflash(reflash[tdir][col])
        csal_tb, csal_util = load_reflash(csal_files[tdir])
        rd, _ = compute_divergence_rel(dp_tb, dp_ratios, ref_tb, ref_util)
        cd, _ = compute_divergence_rel(dp_tb, dp_ratios, csal_tb, csal_util)
        ref_d[col].append(rd)
        csal_d[col].append(cd)
        print(f"{label:<12} {qlc_labels[col]:<8} {rd:>11.2f}% {cd:>11.2f}%")
print("-" * 50)
for col, qlc in enumerate(qlc_suffixes):
    print(f"{'AVERAGE':<12} {qlc_labels[col]:<8} {np.nanmean(ref_d[col]):>11.2f}% {np.nanmean(csal_d[col]):>11.2f}%")


# =========================================================================
# Detailed: Print the time-series of absolute diff for FIO r=8.64 to understand outlier
# =========================================================================
print()
print("=" * 100)
print("DETAIL: FIO r=8.64 trajectory comparison (sampled every 50 steps)")
print("=" * 100)
dp_tb, dp_ratios = load_dp_path(f'{BASE}/dp_path_cb_11_fio_qlc8.64.csv')
ref_tb, ref_util = load_reflash(reflash['cb_11_fio'][0])
csal_tb, csal_util = load_reflash(csal_files['cb_11_fio'])

t_min = max(dp_tb[0], ref_tb[0])
t_max = min(dp_tb[-1], ref_tb[-1])
mask = (dp_tb >= t_min) & (dp_tb <= t_max)
eval_tb = dp_tb[mask]
dp_vals = dp_ratios[mask]
ref_interp = np.interp(eval_tb, ref_tb, ref_util)
csal_interp = np.interp(eval_tb, csal_tb, csal_util)

print(f"{'TB':>8} {'NearOpt':>10} {'REFlash':>10} {'CSAL':>10} {'ref_diff':>10} {'csal_diff':>10}")
for i in range(0, len(eval_tb), 50):
    t = eval_tb[i]
    d = dp_vals[i]
    r = ref_interp[i]
    c = csal_interp[i]
    print(f"{t:>8.2f} {d:>10.4f} {r:>10.4f} {c:>10.4f} {abs(r-d)/d*100 if d>0.01 else 0:>9.1f}% {abs(c-d)/d*100 if d>0.01 else 0:>9.1f}%")


print()
print("=" * 100)
print("Paper claims:")
print("  REFlash:  6.9% at r=8.64,  7.6% at r=2.88")
print("  CSAL:    24.6% at r=8.64, 19.7% at r=2.88")
print("=" * 100)
