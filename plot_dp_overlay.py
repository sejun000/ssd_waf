#!/usr/bin/env python3
import matplotlib.pyplot as plt
import pandas as pd
import numpy as np
import re

# Read the constrained DP results
df_dp = pd.read_csv('constrained_dp_clean.csv', header=None, names=['time', 'chosen_c', 'base_F_t', 'trans_penalty', 'cumulative_cost'])

# Parse Age_Active_Dynamic log file (stat.log.20250825_235728) 
def parse_stat_log(filename):
    write_sizes = []
    valid_ratios = []
    
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
                    valid_ratio = (valid_blocks / total_cache * 4096 * 4096) * 100 / 100  # Convert % to ratio
                    
                    write_sizes.append(write_size)
                    valid_ratios.append(valid_ratio)
    
    return np.array(write_sizes), np.array(valid_ratios)

# Parse the Age_Active_Dynamic data
write_bytes, valid_ratios = parse_stat_log('stat.log.20250825_235728')
write_gb = write_bytes / (1024 * 1024 * 1024)  # Convert to GB

# Create the overlay plot
fig, ax = plt.subplots(1, 1, figsize=(12, 6))

# Plot Age_Active_Dynamic (existing data)
ax.plot(write_gb, valid_ratios, 'r-', alpha=0.7, linewidth=2, label='Age_Active_Dynamic (Actual)')

# Convert DP timesteps to written bytes and match the scale
dp_written_bytes = df_dp['time'] * 32 * 1024 * 1024  # 32MB per timestep
dp_written_gb = dp_written_bytes / (1024 * 1024 * 1024)  # Convert to GB

# Only plot DP data within the range of actual data
max_actual_gb = write_gb.max()
dp_mask = dp_written_gb <= max_actual_gb
dp_filtered = df_dp[dp_mask]
dp_gb_filtered = dp_written_gb[dp_mask]

# Sample DP data for better visualization
sample_step = max(1, len(dp_filtered) // 1000)  # Sample to ~1000 points
dp_sampled = dp_filtered.iloc[::sample_step]
dp_gb_sampled = dp_gb_filtered.iloc[::sample_step]

# Plot DP optimizer results
ax.plot(dp_gb_sampled, dp_sampled['chosen_c'], 'b-', alpha=0.8, linewidth=2, label='DP Optimizer (Optimal)')

# Set labels and formatting
ax.set_title('Valid Ratio Comparison: Actual vs DP Optimal', fontsize=14, fontweight='bold')
ax.set_xlabel('Written Bytes (GB)')
ax.set_ylabel('Valid Ratio')
ax.set_ylim(0.0, 1.0)
ax.grid(True, alpha=0.3)
ax.legend()

# Add statistics
actual_mean = valid_ratios.mean()
dp_mean = df_dp['chosen_c'].mean()
ax.text(0.02, 0.98, 
        f'Actual avg: {actual_mean:.3f}\nDP avg: {dp_mean:.3f}\nDP range: {max_actual_gb:.0f} GB', 
        transform=ax.transAxes, verticalalignment='top', fontsize=10,
        bbox=dict(boxstyle='round', facecolor='white', alpha=0.8))

plt.tight_layout()
plt.savefig('dp_vs_actual_overlay.png', dpi=300, bbox_inches='tight')
print("Overlay plot saved as dp_vs_actual_overlay.png")

print(f"\n=== Comparison Summary ===")
print(f"Actual (Age_Active_Dynamic):")
print(f"  Average valid ratio: {actual_mean:.3f}")
print(f"  Data range: 0 - {max_actual_gb:.0f} GB")
print(f"DP Optimizer:")
print(f"  Average valid ratio: {dp_mean:.3f}")
print(f"  Comparison range: 0 - {max_actual_gb:.0f} GB")