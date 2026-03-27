import re, csv, matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np
from matplotlib.patches import Patch

TB = 1e12; TIB = 1024**4; WARMUP = 6 * TIB; BLK = 4096
WARMUP_MB = 6 * 1024 * 1024

def parse_stat_line(line):
    m = {}
    for key in ['write_size_to_cache','compacted_blocks','evicted_blocks']:
        match = re.search(rf'{key}:\s*(\d+)', line)
        if match: m[key] = int(match.group(1))
    return m

def get_after_warmup(stat_file):
    wl = None; ll = None
    with open(stat_file) as f:
        for line in f:
            if 'write_size_to_cache' not in line: continue
            vals = parse_stat_line(line)
            if vals.get('write_size_to_cache',0) < WARMUP: wl = vals
            ll = vals
    return {k: ll[k] - wl[k] for k in wl}

def parse_dp(dp_file):
    r = {}
    with open(dp_file) as f:
        for line in f:
            if 'Host write:' in line: r['host'] = float(re.search(r'[\d.]+', line).group())
            elif 'Compaction:' in line: r['compact'] = float(re.search(r'[\d.]+', line).group())
            elif 'Eviction:' in line:
                m = re.search(r'Eviction:\s+([\d.]+)\s+\(x([\d.]+)\s+=\s+([\d.]+)\)', line)
                if m: r['evict'] = float(m.group(1)); r['evict_weighted'] = float(m.group(3))
            elif 'Total:' in line: r['total'] = float(re.search(r'[\d.]+', line).group())
    return r

def get_csal(csv_file):
    with open(csv_file) as f:
        reader = csv.DictReader(f)
        wl = None; ll = None
        for row in reader:
            if float(row['host_write_MB']) < WARMUP_MB: wl = row
            ll = row
    host = (float(ll['host_write_MB']) - float(wl['host_write_MB'])) / 1024 / 1024
    compact = (float(ll['cache_write_MB']) - float(wl['cache_write_MB'])) / 1024 / 1024
    evict = (float(ll['backend_write_MB']) - float(wl['backend_write_MB'])) / 1024 / 1024
    return host, compact, evict

ratios = [2, 4, 6, 8, 10]

# REFlash (periodic)
p_h, p_c, p_er, p_t = [], [], [], []
for r in ratios:
    d = get_after_warmup(f"stat_pr{r}.0")
    h = d['write_size_to_cache'] / TB
    c = d['compacted_blocks'] * BLK / TB
    e = d['evicted_blocks'] * BLK / TB
    p_h.append(h); p_c.append(c); p_er.append(e*r); p_t.append(h+c+e*r)

# NearOpt (DP)
d_h, d_c, d_er, d_t = [], [], [], []
for r in ratios:
    dp = parse_dp(f"cb_11_dwpd2_dp_pr{r}.txt")
    d_h.append(dp['host']); d_c.append(dp['compact'])
    d_er.append(dp['evict_weighted']); d_t.append(dp['total'])

# CSAL
cs_host, cs_compact, cs_evict = get_csal('ftl0_20260225_082514.csv')
cs_h, cs_c, cs_er, cs_t = [], [], [], []
for r in ratios:
    cs_h.append(cs_host); cs_c.append(cs_compact)
    cs_er.append(cs_evict * r); cs_t.append(cs_host + cs_compact + cs_evict * r)

# Colors
c_host = ('#A8C8E8', '#4C82B0', '#2B5C90')   # CSAL, REFlash, NearOpt -- but let's do per-system colors
# System colors: NearOpt=pastel, REFlash=medium, CSAL=muted
c_nearopt = {'host': '#A8C8E8', 'compact': '#A8DDA8', 'evict': '#F0A8A8'}
c_reflash = {'host': '#2B6CB0', 'compact': '#2D8B2D', 'evict': '#C0392B'}
c_csal =    {'host': '#8B7EB8', 'compact': '#B8A0D8', 'evict': '#D8B0D8'}

fig, ax = plt.subplots(figsize=(10, 5.5))
x = np.arange(len(ratios))
w = 0.25

for i in range(len(ratios)):
    norm = d_t[i]
    # NearOpt (pastel, left)
    ax.bar(x[i] - w, d_h[i]/norm, w, color=c_nearopt['host'], edgecolor='white', linewidth=0.5)
    ax.bar(x[i] - w, d_c[i]/norm, w, bottom=d_h[i]/norm, color=c_nearopt['compact'], edgecolor='white', linewidth=0.5)
    ax.bar(x[i] - w, d_er[i]/norm, w, bottom=(d_h[i]+d_c[i])/norm, color=c_nearopt['evict'], edgecolor='white', linewidth=0.5)

    # REFlash (middle)
    ax.bar(x[i], p_h[i]/norm, w, color=c_reflash['host'], edgecolor='white', linewidth=0.5)
    ax.bar(x[i], p_c[i]/norm, w, bottom=p_h[i]/norm, color=c_reflash['compact'], edgecolor='white', linewidth=0.5)
    ax.bar(x[i], p_er[i]/norm, w, bottom=(p_h[i]+p_c[i])/norm, color=c_reflash['evict'], edgecolor='white', linewidth=0.5)
    ax.text(x[i], p_t[i]/norm + 0.02, f'{p_t[i]/norm:.2f}', ha='center', fontsize=9, fontweight='bold', color='#2B6CB0')

    # CSAL (right)
    ax.bar(x[i] + w, cs_h[i]/norm, w, color=c_csal['host'], edgecolor='white', linewidth=0.5)
    ax.bar(x[i] + w, cs_c[i]/norm, w, bottom=cs_h[i]/norm, color=c_csal['compact'], edgecolor='white', linewidth=0.5)
    ax.bar(x[i] + w, cs_er[i]/norm, w, bottom=(cs_h[i]+cs_c[i])/norm, color=c_csal['evict'], edgecolor='white', linewidth=0.5)
    ax.text(x[i] + w, cs_t[i]/norm + 0.02, f'{cs_t[i]/norm:.2f}', ha='center', fontsize=9, fontweight='bold', color='#8B7EB8')

ax.axhline(y=1.0, color='gray', linestyle='--', linewidth=0.8, alpha=0.5)

legend_elements = [
    Patch(facecolor=c_nearopt['host'], label='NearOpt'),
    Patch(facecolor=c_reflash['host'], label='REFlash'),
    Patch(facecolor=c_csal['host'], label='CSAL'),
    Patch(facecolor='white', label=''),
    Patch(facecolor='#5588BB', label='Host Write'),
    Patch(facecolor='#55AA55', label='Compaction'),
    Patch(facecolor='#CC6666', label='Eviction'),
]

ax.set_xticks(x)
ax.set_xticklabels([f'r={r}' for r in ratios], fontsize=12)
ax.set_ylabel('Normalized Total Write Cost', fontsize=12)
ax.legend(handles=legend_elements, fontsize=9, loc='upper left', ncol=2)
ax.grid(True, alpha=0.2, axis='y')

plt.tight_layout()
plt.savefig('periodic_vs_dp_plot.pdf', bbox_inches='tight')
plt.savefig('periodic_vs_dp_plot.png', dpi=150, bbox_inches='tight')
print("Saved: periodic_vs_dp_plot.pdf / .png")
