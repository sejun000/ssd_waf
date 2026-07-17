#!/usr/bin/env python3
"""F_frac avg-per-flush hl=1·seg sweep: stacked bar (host / GC / r·flush)
normalized to D=1 TEC."""
import os, re
import matplotlib.pyplot as plt
import numpy as np

BLK = 4096
KEYS = ('write_size_to_cache','global_valid_blocks','total_cache_size',
        'evicted_blocks','compacted_blocks','gc_victim_count')

def parse(path):
    if not os.path.exists(path): return None
    last = None
    for ln in open(path):
        if 'LOG_GREEDY' in ln and 'write_size_to_cache' in ln: last = ln
    if last is None: return None
    return {k:int(re.search(rf'{k}:?\s+(\d+)',last).group(1)) for k in KEYS
            if re.search(rf'{k}:?\s+(\d+)',last)}

Ds = [1, 2, 4, 8, 16]
configs = [(8.64, '864'), (2.88, '288')]

FS = 20
plt.rcParams.update({'font.size': FS})

# High-contrast palette
COLOR_HOST  = '#003f5c'   # deep navy
COLOR_GC    = '#ffa600'   # vivid orange
COLOR_FLUSH = '#d62728'   # crimson

fig, axes = plt.subplots(1, 2, figsize=(14, 6.5), sharey=True)
labels = ['(a) r = 8.64', '(b) r = 2.88']
bars_for_legend = None
for ax, (r, rt), sublabel in zip(axes, configs, labels):
    rows = []
    for D in Ds:
        F = f'/home/sejun000/ssd_waf/LOG_GREEDY_COST_BENEFIT_10_GS_FINAL_us02_ewma_hl1572864_gsdec{rt}finald{D}_d{D}.stat_pr{rt}'
        d = parse(F)
        if d is None:
            rows.append((D, 0, 0, 0))
            continue
        host = d['write_size_to_cache']/1e12
        comp = d['compacted_blocks']*BLK/1e12
        evict = d['evicted_blocks']*BLK/1e12
        rows.append((D, host, comp, r*evict))

    Ds_v = [x[0] for x in rows]
    host = np.array([x[1] for x in rows])
    gc = np.array([x[2] for x in rows])
    rflush = np.array([x[3] for x in rows])
    tec_d1 = host[0] + gc[0] + rflush[0]

    host_n = host / tec_d1
    gc_n = gc / tec_d1
    rflush_n = rflush / tec_d1
    tec_n = host_n + gc_n + rflush_n

    x = np.arange(len(Ds_v))
    width = 0.6
    b1 = ax.bar(x, host_n, width, label='host write', color=COLOR_HOST)
    b2 = ax.bar(x, gc_n, width, bottom=host_n, label='GC (compaction)', color=COLOR_GC)
    b3 = ax.bar(x, rflush_n, width, bottom=host_n+gc_n, label='r·flush', color=COLOR_FLUSH)
    bars_for_legend = (b1, b2, b3)

    for i, t in enumerate(tec_n):
        ax.text(i, t + 0.01, f'{t:.3f}', ha='center', va='bottom', fontsize=FS-4)

    ax.set_xticks(x)
    ax.set_xticklabels([f'$\\delta={d}$' for d in Ds_v])
    ax.set_xlabel(sublabel)
    ax.axhline(1.0, color='gray', linestyle='--', linewidth=0.8, alpha=0.6)
    ax.set_ylim(0, max(tec_n.max(), 1.0) * 1.15)
    ax.grid(axis='y', alpha=0.3)

axes[0].set_ylabel('Normalized TEC')

fig.legend(bars_for_legend, ['host write', 'GC (compaction)', 'r·flush'],
           loc='upper center', bbox_to_anchor=(0.5, 0.99),
           ncol=3, frameon=False, fontsize=FS-2)

plt.tight_layout(rect=(0, 0, 1, 0.92))
plt.subplots_adjust(wspace=0.05)
out = '/home/sejun000/ssd_waf/A_ffrac_hl1_norm.pdf'
plt.savefig(out)
plt.savefig(out.replace('.pdf', '.png'), dpi=120)
print(f'saved: {out} (+ .png)')
