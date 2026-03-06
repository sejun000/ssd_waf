#!/usr/bin/env python3
"""Plot periodic vs DP-predicted cumulative writes."""

import pandas as pd
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker

df = pd.read_csv("periodic_vs_dp_cumulative.csv")

fig, axes = plt.subplots(3, 1, figsize=(12, 10), sharex=True)

# ── 1. GC (compacted) ──────────────────────────────────────
ax = axes[0]
ax.plot(df['ws_tb'], df['p_gc_gb'],  linewidth=1.5, label='Periodic (actual)')
ax.plot(df['ws_tb'], df['dp_gc_gb'], linewidth=1.5, label='DP predicted', linestyle='--')
ax.fill_between(df['ws_tb'], df['dp_gc_gb'], df['p_gc_gb'], alpha=0.15, color='C0')
ax.set_ylabel('Cumulative GC writes (GB)')
ax.legend(loc='upper left')
ax.set_title('Periodic vs DP-predicted cumulative writes')
ax.grid(True, alpha=0.3)

# ── 2. Flush (evicted) ─────────────────────────────────────
ax = axes[1]
ax.plot(df['ws_tb'], df['p_flush_gb'],  linewidth=1.5, label='Periodic (actual)')
ax.plot(df['ws_tb'], df['dp_flush_gb'], linewidth=1.5, label='DP predicted', linestyle='--')
ax.fill_between(df['ws_tb'], df['dp_flush_gb'], df['p_flush_gb'], alpha=0.15, color='C1')
ax.set_ylabel('Cumulative Flush writes (GB)')
ax.legend(loc='upper left')
ax.grid(True, alpha=0.3)

# ── 3. Total + diff% on twin axis ──────────────────────────
ax = axes[2]
p_tot = df['p_total_gb']
d_tot = df['dp_total_gb']
ax.plot(df['ws_tb'], p_tot, linewidth=1.5, label='Periodic (actual)')
ax.plot(df['ws_tb'], d_tot, linewidth=1.5, label='DP predicted', linestyle='--')
ax.fill_between(df['ws_tb'], d_tot, p_tot, alpha=0.15, color='C2')
ax.set_ylabel('Cumulative Total writes (GB)')
ax.set_xlabel('Host writes to cache (TB)')
ax.legend(loc='upper left')
ax.grid(True, alpha=0.3)

# diff % on twin y-axis
ax2 = ax.twinx()
mask = d_tot > 1  # avoid div-by-zero early on
diff_pct = ((p_tot - d_tot) / d_tot * 100).where(mask)
ax2.plot(df['ws_tb'], diff_pct, color='red', linewidth=1.0, alpha=0.6, label='diff %')
ax2.set_ylabel('Periodic − DP (%)', color='red')
ax2.tick_params(axis='y', labelcolor='red')
ax2.set_ylim(-10, 30)
ax2.axhline(0, color='red', linewidth=0.5, linestyle=':')
ax2.legend(loc='upper right')

plt.tight_layout()
plt.savefig('periodic_vs_dp_plot.pdf', bbox_inches='tight')
plt.savefig('periodic_vs_dp_plot.png', dpi=150, bbox_inches='tight')
print("Saved: periodic_vs_dp_plot.pdf, periodic_vs_dp_plot.png")
