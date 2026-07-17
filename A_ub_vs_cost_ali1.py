#!/usr/bin/env python3
# Per-row scatter: x = U_B = global_valid_blocks / (total_cache_size/4096),
#                  y = Δcompacted_blocks + 8.64·Δevicted_blocks.
# Source: dp.0.* sweep files in /home/sejun000/ssd_waf (ali1 = dwpd2_5x).
import glob, re
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

PAGE = 4096
QLC = 8.64
BASE = '/home/sejun000/ssd_waf'

xs, ys = [], []
n_files = 0
for f in sorted(glob.glob(f'{BASE}/dp.0.*')):
    n_files += 1
    prev_c = prev_e = None
    with open(f) as fh:
        for line in fh:
            if 'invalidate_blocks:' not in line:
                continue
            m_g = re.search(r'global_valid_blocks:\s*(\d+)', line)
            m_t = re.search(r'total_cache_size:\s*(\d+)', line)
            m_c = re.search(r'compacted_blocks:\s*(\d+)', line)
            m_e = re.search(r'evicted_blocks:\s*(\d+)', line)
            if not (m_g and m_t and m_c and m_e):
                continue
            g = int(m_g.group(1))
            t = int(m_t.group(1))
            c = int(m_c.group(1))
            e = int(m_e.group(1))
            if t <= 0:
                continue
            cache_blocks = t / PAGE
            ub = g / cache_blocks
            if prev_c is None:
                prev_c, prev_e = c, e
                continue
            dc = c - prev_c
            de = e - prev_e
            prev_c, prev_e = c, e
            if dc < 0 or de < 0:
                continue
            xs.append(ub)
            ys.append(dc + QLC * de)

print(f"files={n_files}, points={len(xs)}")

plt.rcParams.update({'font.size': 14})
fig, ax = plt.subplots(figsize=(10, 6))
ax.scatter(xs, ys, s=2, alpha=0.25, color='tab:blue', edgecolors='none')
ax.set_xlabel(r'$U_B$ = global_valid_blocks / (total_cache_size / 4096)')
ax.set_ylabel(r'$\Delta$compacted + 8.64$\cdot\Delta$evicted (blocks per 6 GiB step)')
ax.set_title(f'Ali1 (dwpd2_5x) — per-row GC cost vs $U_B$  ({n_files} files, {len(xs)} points)')
ax.set_xlim(0, 1.0)
ax.grid(True, alpha=0.3, linestyle='--')
ax.set_axisbelow(True)

plt.tight_layout()
out_pdf = f'{BASE}/A_ub_vs_cost_ali1.pdf'
out_png = f'{BASE}/A_ub_vs_cost_ali1.png'
plt.savefig(out_pdf, dpi=150)
plt.savefig(out_png, dpi=150)
print(f'Saved: {out_pdf}\nSaved: {out_png}')
