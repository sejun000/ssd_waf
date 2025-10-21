#!/usr/bin/env python3
import matplotlib.pyplot as plt
import pandas as pd
import numpy as np

# Read the constrained DP results
df = pd.read_csv('constrained_dp_clean.csv', header=None, names=['time', 'chosen_c', 'base_F_t', 'trans_penalty', 'cumulative_cost'])

# Create single plot like the valid block rate plot
fig, ax = plt.subplots(1, 1, figsize=(10, 6))

# Sample data for performance (every 100th point)
sample_indices = np.arange(0, len(df), 100)
sampled_df = df.iloc[sample_indices]

# Convert time steps to written bytes (32MB per step)
written_bytes = sampled_df['time'] * 32 * 1024 * 1024  # 32MB per timestep
written_gb = written_bytes / (1024 * 1024 * 1024)  # Convert to GB

# Plot time series of chosen valid ratio
ax.plot(written_gb, sampled_df['chosen_c'], 'b-', alpha=0.8, linewidth=1.5, label='DP Optimal Choice')

# Set title and labels similar to plot.sh style  
ax.set_title('DP Optimizer: Valid Ratio Selection Over Time', fontsize=14, fontweight='bold')
ax.set_xlabel('Written Bytes (GB)')
ax.set_ylabel('Valid Ratio')

# Set y-axis to 0-1.0 range as requested
ax.set_ylim(0.0, 1.0)

# Add grid and formatting
ax.grid(True, alpha=0.3)
ax.legend()

# Add some statistics as text
total_changes = (df['chosen_c'].diff() != 0).sum()
most_used_ratio = df['chosen_c'].mode()[0]
ax.text(0.02, 0.98, f'Changes: {total_changes:,}\nMost used: {most_used_ratio:.2f}', 
        transform=ax.transAxes, verticalalignment='top', fontsize=10,
        bbox=dict(boxstyle='round', facecolor='white', alpha=0.8))

plt.tight_layout()
plt.savefig('dp_optimal_ratio_timeseries.png', dpi=300, bbox_inches='tight')
print("Simple DP timeseries plot saved as dp_optimal_ratio_timeseries.png")

# Print summary stats
print(f"\n=== DP Optimizer Summary ===")
print(f"Time steps: {len(df):,}")
print(f"Valid ratio changes: {total_changes:,}")
print(f"Most used ratio: {most_used_ratio:.2f}")
print(f"Ratio range: {df['chosen_c'].min():.2f} - {df['chosen_c'].max():.2f}")