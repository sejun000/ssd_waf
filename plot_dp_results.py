#!/usr/bin/env python3
import pandas as pd
import matplotlib.pyplot as plt
import numpy as np

# Read the data
df = pd.read_csv('dp_plot_data.csv')

# Create figure with subplots
fig, axes = plt.subplots(2, 2, figsize=(15, 10))
fig.suptitle('DP Optimizer Results Analysis', fontsize=16)

# 1. Valid Ratio over time
axes[0,0].plot(df['t'], df['chosen_c'], color='blue', alpha=0.7)
axes[0,0].set_title('Valid Ratio Selection Over Time')
axes[0,0].set_xlabel('Time Step')
axes[0,0].set_ylabel('Valid Ratio')
axes[0,0].grid(True, alpha=0.3)

# 2. Base F_t (cost) over time (only non-zero values)
non_zero_f = df[df['base_F_t'] > 0]
if len(non_zero_f) > 0:
    axes[0,1].plot(non_zero_f['t'], non_zero_f['base_F_t'], color='red', alpha=0.7)
    axes[0,1].set_title('Base Cost F_t Over Time (Non-zero only)')
    axes[0,1].set_xlabel('Time Step')
    axes[0,1].set_ylabel('Base Cost F_t')
    axes[0,1].grid(True, alpha=0.3)
else:
    axes[0,1].text(0.5, 0.5, 'All F_t values are zero', ha='center', va='center', transform=axes[0,1].transAxes)
    axes[0,1].set_title('Base Cost F_t Over Time')

# 3. Transition Penalty over time (only non-zero values)
non_zero_penalty = df[df['trans_penalty'] > 0]
if len(non_zero_penalty) > 0:
    axes[1,0].plot(non_zero_penalty['t'], non_zero_penalty['trans_penalty'], color='orange', alpha=0.7)
    axes[1,0].set_title('Transition Penalty Over Time (Non-zero only)')
    axes[1,0].set_xlabel('Time Step')
    axes[1,0].set_ylabel('Transition Penalty')
    axes[1,0].grid(True, alpha=0.3)
else:
    axes[1,0].text(0.5, 0.5, 'All penalties are zero', ha='center', va='center', transform=axes[1,0].transAxes)
    axes[1,0].set_title('Transition Penalty Over Time')

# 4. Cumulative Cost over time
axes[1,1].plot(df['t'], df['cumulative_cost'], color='green', alpha=0.7)
axes[1,1].set_title('Cumulative Cost Over Time')
axes[1,1].set_xlabel('Time Step')
axes[1,1].set_ylabel('Cumulative Cost')
axes[1,1].grid(True, alpha=0.3)

plt.tight_layout()
plt.savefig('dp_optimizer_analysis.png', dpi=150, bbox_inches='tight')
plt.show()

print(f"Processed {len(df)} time steps")
print(f"Valid ratios used: {sorted(df['chosen_c'].unique())}")
print(f"Ratio changes: {(df['chosen_c'].diff() != 0).sum() - 1}")  # -1 for first step
print(f"Non-zero F_t values: {(df['base_F_t'] > 0).sum()}")
print(f"Non-zero penalties: {(df['trans_penalty'] > 0).sum()}")