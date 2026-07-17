#!/usr/bin/env python3
# Plot NearOpt U_B trajectory from new dp_optimizer outputs.
# X: host write from 0 TB.
# Y: chosen U_B — warmup section uses per-row sample minimum (matches DP start anchor),
#                 post-warmup section uses dp_optimizer DP path.
import glob
import re
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

BASE = '/home/sejun000/ssd_waf'
PAGE = 4096
TIB = 1024 ** 4
WARMUP_TB = 6.0
WARMUP_BYTES = int(WARMUP_TB * TIB)

STAT = (f'{BASE}/LOG_GREEDY_COST_BENEFIT_10_GS_FINAL_us02_ewma_hl1572864_'
        f'gsdec864PrGhD1_{{tag}}.stat_pr864')

# (label, dp_dir, dp_v2_output, stat_tag)
traces = [
    ('Ali1 (dwpd2_5x)',    f'{BASE}/cb_11_dwpd1',    f'{BASE}/cb_11_dwpd1_dp_v2.txt',    'dwpd3_d1'),
    ('Ali2 (dwpd1to2_4x)', f'{BASE}/cb_11_dwpd2',    f'{BASE}/cb_11_dwpd2_dp_v2.txt',    'd1'),
    ('Ali3 (dwpd01to1)',   f'{BASE}/cb_11_dwpd3',    f'{BASE}/cb_11_dwpd3_dp_v2.txt',    'dwpd1_d1'),
    ('YCSB-A',             f'{BASE}/cb_11_ssdtrace', f'{BASE}/cb_11_ssdtrace_dp_v2.txt', 'ssdtr_d1'),
]


def load_stat_path(stat_path):
    """Parse REFlash stat file: each line contains
       global_valid_blocks: G  write_size_to_cache: W  total_cache_size: T  ...
       Returns (host_TB, actual_UB) starting from host write 0."""
    host_tb, ub = [], []
    pat_w = re.compile(r'write_size_to_cache:\s*(\d+)')
    pat_g = re.compile(r'global_valid_blocks:\s*(\d+)')
    pat_t = re.compile(r'total_cache_size:\s*(\d+)')
    with open(stat_path) as f:
        for line in f:
            if 'invalidate_blocks:' not in line:
                continue
            mw, mg, mt = pat_w.search(line), pat_g.search(line), pat_t.search(line)
            if not (mw and mg and mt):
                continue
            w = int(mw.group(1)); g = int(mg.group(1)); t = int(mt.group(1))
            if t <= 0:
                continue
            host_tb.append(w / TIB)
            ub.append(g * PAGE / t)
    return host_tb, ub


def load_warmup_min(trace_dir):
    """For each row in [0, WARMUP_BYTES), take min actual_UB across all dp.* files
    (same anchor rule as DP start)."""
    per_file_rows = []
    for f in sorted(glob.glob(f'{trace_dir}/dp.*')):
        rows = []
        with open(f) as fh:
            for line in fh:
                if 'invalidate_blocks:' not in line:
                    continue
                w_m = re.search(r'write_size_to_cache:\s*(\d+)', line)
                g_m = re.search(r'global_valid_blocks:\s*(\d+)', line)
                t_m = re.search(r'total_cache_size:\s*(\d+)', line)
                if not (w_m and g_m and t_m):
                    continue
                w = int(w_m.group(1))
                if w >= WARMUP_BYTES:
                    break
                gvb = int(g_m.group(1))
                tc = int(t_m.group(1))
                if tc <= 0:
                    continue
                rows.append((w / TIB, gvb * PAGE / tc))
        if rows:
            per_file_rows.append(rows)
    if not per_file_rows:
        return [], []
    min_len = min(len(r) for r in per_file_rows)
    out_tb, out_ub = [], []
    for i in range(min_len):
        host_tb = sum(r[i][0] for r in per_file_rows) / len(per_file_rows)
        ub_min = min(r[i][1] for r in per_file_rows)
        out_tb.append(host_tb)
        out_ub.append(ub_min)
    return out_tb, out_ub


def load_dp_path(path):
    """Parse `t, host_TB, chosen_c, base_F_t, cumulative_cost` rows."""
    host_tb, ratios = [], []
    in_data = False
    with open(path) as f:
        for line in f:
            line = line.rstrip()
            if line.startswith('t, host_TB,'):
                in_data = True
                continue
            if not in_data:
                continue
            if not line or line.startswith('==='):
                break
            parts = [p.strip() for p in line.split(',')]
            if len(parts) < 3:
                continue
            try:
                host_tb.append(float(parts[1]))
                ratios.append(float(parts[2]))
            except ValueError:
                continue
    return host_tb, ratios


plt.rcParams.update({'font.size': 16})
fig, axes = plt.subplots(len(traces), 1, figsize=(12, 3.0 * len(traces)), sharex=True)
if len(traces) == 1:
    axes = [axes]

xmax = 0.0
for _, _, dp_path, _ in traces:
    tb, _ = load_dp_path(dp_path)
    if tb:
        xmax = max(xmax, tb[-1])

handles_for_legend, labels_for_legend = None, None
for ax, (label, trace_dir, dp_path, stat_tag) in zip(axes, traces):
    w_tb, w_ub = load_warmup_min(trace_dir)
    d_tb, d_ub = load_dp_path(dp_path)
    near_tb = w_tb + d_tb
    near_ub = w_ub + d_ub

    stat_path = STAT.format(tag=stat_tag)
    s_tb, s_ub = ([], [])
    try:
        s_tb, s_ub = load_stat_path(stat_path)
    except FileNotFoundError:
        pass

    l_ref = None
    if s_tb:
        l_ref, = ax.plot(s_tb, s_ub, color='tab:red', linewidth=1.8, alpha=0.85,
                         label='REFLASH')
    l_near = None
    if near_tb:
        l_near, = ax.plot(near_tb, near_ub, color='tab:blue', linewidth=2.0,
                          label='NearOpt')
    ax.axvline(x=WARMUP_TB, color='gray', linestyle=':', linewidth=1.0, alpha=0.6)
    ax.set_xlim(0, xmax)
    ax.set_ylim(0.0, 1.0)
    ax.set_yticks([0.0, 0.25, 0.5, 0.75, 1.0])
    ax.axhline(y=0.75, color='gray', linestyle=':', linewidth=1.0, alpha=0.6)
    ax.axhline(y=0.25, color='gray', linestyle=':', linewidth=1.0, alpha=0.6)
    ax.grid(True, alpha=0.3, color='black', linestyle='--')
    ax.set_axisbelow(True)
    ax.text(0.02, 0.08, label, transform=ax.transAxes,
            fontsize=18, fontweight='bold')
    ax.set_ylabel(r'$U_B$', fontsize=20)

    if handles_for_legend is None:
        h, lab = [], []
        if l_ref is not None:
            h.append(l_ref); lab.append('REFLASH')
        if l_near is not None:
            h.append(l_near); lab.append('NearOpt')
        handles_for_legend, labels_for_legend = h, lab

if handles_for_legend:
    axes[0].legend(handles_for_legend, labels_for_legend,
                   loc='lower center', bbox_to_anchor=(0.5, 1.02),
                   ncol=len(handles_for_legend), frameon=False, fontsize=16)

axes[-1].set_xlabel('Host Write (TB)  [from 0]', fontsize=16)

plt.tight_layout()
out_pdf = f'{BASE}/A_dp_path_v2.pdf'
out_png = f'{BASE}/A_dp_path_v2.png'
plt.savefig(out_pdf, dpi=150)
plt.savefig(out_png, dpi=150)
print(f'Saved: {out_pdf}\nSaved: {out_png}')
