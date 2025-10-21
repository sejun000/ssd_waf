#!/usr/bin/env python3
import matplotlib.pyplot as plt
import pandas as pd
import numpy as np
import re

# Read the fresh DP results
df_dp = pd.read_csv('fresh_dp_clean.csv', header=None, names=['time', 'chosen_c', 'base_F_t', 'trans_penalty', 'cumulative_cost'])

# Parse stat log file
def parse_stat_log(filename):
    write_sizes = []
    valid_ratios = []
    
    try:
        with open(filename, 'r') as f:
            for line in f:
                if 'write_size_to_cache:' in line:
                    # Extract values using regex
                    write_match = re.search(r'write_size_to_cache: (\d+)', line)
                    valid_match = re.search(r'global_valid_blocks: (\d+)', line)
                    cache_match = re.search(r'total_cache_size: (\d+)', line)
                    
                    if write_match and valid_match and cache_match:
                        write_size = int(write_match.group(1))
                        valid_blocks = int(valid_match.group(1))
                        total_cache = int(cache_match.group(1))
                        
                        # Calculate valid ratio (matching plot.sh formula)
                        valid_ratio = (valid_blocks / total_cache * 4096 * 4096)
                        
                        write_sizes.append(write_size)
                        valid_ratios.append(valid_ratio)
    except FileNotFoundError:
        print(f"Warning: {filename} not found")
        return np.array([]), np.array([])
    
    return np.array(write_sizes), np.array(valid_ratios)

# File names and labels from plot.sh
files = [
    'stat.log.20250818_214512',
    'stat.log.20250818_231203', 
    'stat.log.20250818_210101',
    'stat.log.20250818_223352',
    'stat.log.20250825_235728'
]

labels = [
    'FIFO',
    'Greedy_80%',
    'Age_Active_93%', 
    'Age_Active_80%',
    'Age_Active_Dynamic'
]

colors = ['green', 'orange', 'purple', 'brown', 'red']

# Create the overlay plot
fig, ax = plt.subplots(1, 1, figsize=(14, 8))

# Plot all actual policies
max_gb = 0
for i, (filename, label, color) in enumerate(zip(files, labels, colors)):
    write_bytes, valid_ratios = parse_stat_log(filename)
    if len(write_bytes) > 0:
        write_gb = write_bytes / (1024 * 1024 * 1024)  # Convert to GB
        max_gb = max(max_gb, write_gb.max())
        
        # Sample data for better visualization
        sample_step = max(1, len(write_gb) // 2000)
        ax.plot(write_gb[::sample_step], valid_ratios[::sample_step], 
                color=color, alpha=0.7, linewidth=1.5, label=f'{label} (Actual)')
        
        print(f"{label}: {len(write_gb)} points, max {write_gb.max():.0f} GB, avg valid ratio {valid_ratios.mean():.3f}")

# Convert DP timesteps to written bytes
dp_written_bytes = df_dp['time'] * 32 * 1024 * 1024  # 32MB per timestep
dp_written_gb = dp_written_bytes / (1024 * 1024 * 1024)  # Convert to GB

# Only plot DP data within the range of actual data
dp_mask = dp_written_gb <= max_gb
dp_filtered = df_dp[dp_mask]
dp_gb_filtered = dp_written_gb[dp_mask]

# Sample DP data for better visualization
sample_step = max(1, len(dp_filtered) // 1000)
dp_sampled = dp_filtered.iloc[::sample_step]
dp_gb_sampled = dp_gb_filtered.iloc[::sample_step]

# Plot DP optimizer results with thicker line
ax.plot(dp_gb_sampled, dp_sampled['chosen_c'], 'blue', alpha=0.9, linewidth=3, 
        label='DP Optimizer (Optimal)', zorder=10)

# Set labels and formatting
ax.set_title('Valid Ratio Comparison: All Policies vs DP Optimal', fontsize=14, fontweight='bold')
ax.set_xlabel('Written Bytes (GB)')
ax.set_ylabel('Valid Ratio')
ax.set_ylim(0.0, 1.0)
ax.grid(True, alpha=0.3)
ax.legend(bbox_to_anchor=(1.05, 1), loc='upper left')

# Add statistics
dp_mean = df_dp['chosen_c'].mean()
ax.text(0.02, 0.98, 
        f'DP Optimizer avg: {dp_mean:.3f}\nData range: 0 - {max_gb:.0f} GB', 
        transform=ax.transAxes, verticalalignment='top', fontsize=10,
        bbox=dict(boxstyle='round', facecolor='lightblue', alpha=0.8))

plt.tight_layout()
plt.savefig('dp_vs_all_policies_overlay_fresh.png', dpi=300, bbox_inches='tight')
print(f"\nOverlay plot with all policies saved as dp_vs_all_policies_overlay_fresh.png")
print(f"DP Optimizer average: {dp_mean:.3f}")
print(f"Data range: 0 - {max_gb:.0f} GB")