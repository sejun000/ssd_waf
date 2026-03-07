#!/usr/bin/env python3
"""Plot periodic vs DP-predicted cumulative writes."""

import pandas as pd
import matplotlib.pyplot as plt

plt.rcParams.update({'font.size': 24})

df = pd.read_csv("periodic_vs_dp_cumulative.csv")
ws_gb = df['ws_tb'] * 1000

fig, axes = plt.subplots(1, 2, figsize=(20, 7), sharey=False)

# ── 1. Buffer-tier writes (GC + Host) ──────────────────────
ax = axes[0]
p_slc = df['p_gc_gb'] + ws_gb
d_slc = df['dp_gc_gb'] + ws_gb
ax.plot(df['ws_tb'], p_slc,  linewidth=2, label='Single experiment')
ax.plot(df['ws_tb'], d_slc, linewidth=2, label='Predicted (fixed-BUtil sweep)', linestyle='--')
ax.set_ylabel('Buffer-tier writes (GB)')
ax.set_xlabel('Host writes to cache (TB)')
ax.grid(True, alpha=0.3)

# ── 2. Flush-tier writes (evicted) ─────────────────────────
ax = axes[1]
ax.plot(df['ws_tb'], df['p_flush_gb'],  linewidth=2, label='Single experiment')
ax.plot(df['ws_tb'], df['dp_flush_gb'], linewidth=2, label='Predicted (fixed-BUtil sweep)', linestyle='--')
ax.set_ylabel('Flush-tier writes (GB)')
ax.set_xlabel('Host writes to cache (TB)')
ax.grid(True, alpha=0.3)

# ── Shared legend at top center, no border ──────────────────
handles, labels = axes[0].get_legend_handles_labels()
fig.legend(handles, labels, loc='upper center', ncol=2,
           frameon=False, fontsize=24, bbox_to_anchor=(0.5, 1.02))

plt.tight_layout(rect=[0, 0, 1, 0.93])
plt.savefig('periodic_vs_dp_plot.pdf', bbox_inches='tight')
plt.savefig('periodic_vs_dp_plot.png', dpi=150, bbox_inches='tight')
print("Saved: periodic_vs_dp_plot.pdf, periodic_vs_dp_plot.png")
