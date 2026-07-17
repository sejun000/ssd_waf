#!/usr/bin/env python3
# NearOpt U_B trajectory (dp_optimizer free-start, warmup_tb=6) vs REFLASH (GS_FINAL r=8.64).
# 4 workloads (Ali1, Ali2, Ali3, YCSB-A) — one row each, host write on x-axis.
import glob, re
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

BASE = '/home/sejun000/ssd_waf'
PAGE = 4096
TIB = 1024 ** 4
WARMUP_TB = 6.0
WARMUP_BYTES = int(WARMUP_TB * TIB)

# (label, dp_dir, dp_optimizer_out, REFLASH stat tag for GS_FINAL_clean_*.stat_pr864)
traces = [
    ('Ali1',   f'{BASE}/ali1_dp_cb11', f'{BASE}/dp_ali1.out',     'dwpd2_5x'),
    ('Ali2',   f'{BASE}/ali2_dp_cb11', f'{BASE}/dp_ali2.out',     'dwpd1to2'),
    ('Ali3',   f'{BASE}/ali3_dp',      f'{BASE}/dp_ali3.out',     'dwpd01to1'),
    ('YCSB-A', f'{BASE}/ssdtrace_dp',  f'{BASE}/dp_ssdtrace.out', 'scaled4x'),
]


def load_reflash(stat_path):
    """REFLASH r=8.64 trajectory: (host_TB, actual_UB) from invalidate_blocks rows,
    filtered to host write >= WARMUP_TB (same warmup cut as dp_optimizer --warmup_tb 6)."""
    host_tb, ub = [], []
    pw = re.compile(r'write_size_to_cache:\s*(\d+)')
    pg = re.compile(r'global_valid_blocks:\s*(\d+)')
    pt = re.compile(r'total_cache_size:\s*(\d+)')
    with open(stat_path) as f:
        for line in f:
            if 'invalidate_blocks:' not in line:
                continue
            mw, mg, mt = pw.search(line), pg.search(line), pt.search(line)
            if not (mw and mg and mt):
                continue
            w = int(mw.group(1)); g = int(mg.group(1)); t = int(mt.group(1))
            if t <= 0 or w < WARMUP_BYTES:
                continue
            host_tb.append(w / TIB)
            ub.append(g * PAGE / t)
    return host_tb, ub


def load_dp_path(path):
    """Parse `t, host_TB, chosen_c, base_F_t, cumulative_cost` rows from dp_optimizer."""
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
fig, axes = plt.subplots(len(traces), 1, figsize=(12, 2.8 * len(traces)), sharex=True)
if len(traces) == 1:
    axes = [axes]

xmax = 0.0
for _, _, dp_path, _ in traces:
    tb, _ = load_dp_path(dp_path)
    if tb:
        xmax = max(xmax, tb[-1])

handles, labels = None, None
for ax, (label, trace_dir, dp_path, stat_tag) in zip(axes, traces):
    near_tb, near_ub = load_dp_path(dp_path)

    stat_path = f'{BASE}/GS_FINAL_clean_{stat_tag}.stat_pr864'
    s_tb, s_ub = ([], [])
    try:
        s_tb, s_ub = load_reflash(stat_path)
    except FileNotFoundError:
        pass

    l_ref = l_near = None
    if s_tb:
        l_ref, = ax.plot(s_tb, s_ub, color='tab:red', linewidth=1.8, alpha=0.85,
                         label='REFLASH (r=8.64)')
    if near_tb:
        l_near, = ax.plot(near_tb, near_ub, color='tab:blue', linewidth=2.0,
                          label='NearOpt')
    ax.set_xlim(WARMUP_TB, xmax)
    ax.set_ylim(0.0, 1.0)
    ax.set_yticks([0.0, 0.25, 0.5, 0.75, 1.0])
    ax.axhline(y=0.75, color='gray', linestyle=':', linewidth=1.0, alpha=0.6)
    ax.axhline(y=0.25, color='gray', linestyle=':', linewidth=1.0, alpha=0.6)
    ax.grid(True, alpha=0.3, color='black', linestyle='--')
    ax.set_axisbelow(True)
    ax.text(0.02, 0.08, label, transform=ax.transAxes,
            fontsize=18, fontweight='bold')
    ax.set_ylabel(r'$U_B$', fontsize=20)

    if handles is None:
        h, lab = [], []
        if l_ref is not None: h.append(l_ref); lab.append('REFLASH (r=8.64)')
        if l_near is not None: h.append(l_near); lab.append('NearOpt')
        handles, labels = h, lab

if handles:
    axes[0].legend(handles, labels,
                   loc='lower center', bbox_to_anchor=(0.5, 1.02),
                   ncol=len(handles), frameon=False, fontsize=16)

axes[-1].set_xlabel('Host Write (TB)', fontsize=16)

plt.tight_layout()
out_pdf = f'{BASE}/A_dp_path_plot.pdf'
out_png = f'{BASE}/A_dp_path_plot.png'
plt.savefig(out_pdf, dpi=150)
plt.savefig(out_png, dpi=150)
print(f'Saved: {out_pdf}')
print(f'Saved: {out_png}')
