#!/usr/bin/env python3
import pandas as pd
import re

# Files and labels from the plotting script
files = [
    'stat.log.20250818_214512',  # FIFO
    'stat.log.20250818_231203',  # Greedy_80%
    'stat.log.20250818_210101',  # Age_Active_93%
    'stat.log.20250818_223352',  # Age_Active_80%
    'stat.log.20250825_235728'   # Age_Active_Dynamic
]

labels = [
    'FIFO',
    'Greedy_80%',
    'Age_Active_93%', 
    'Age_Active_80%',
    'Age_Active_Dynamic'
]

def get_final_stats(filename):
    """Extract final statistics from stat log file"""
    try:
        with open(filename, 'r') as f:
            lines = f.readlines()
            
        # Find the last line with write_size_to_cache
        for line in reversed(lines):
            if 'write_size_to_cache:' in line:
                # Extract values using regex
                write_match = re.search(r'write_size_to_cache: (\d+)', line)
                compacted_match = re.search(r'compacted_blocks: (\d+)', line)
                evicted_match = re.search(r'evicted_blocks: (\d+)', line)
                
                if write_match and compacted_match and evicted_match:
                    write_size = int(write_match.group(1))
                    compacted_blocks = int(compacted_match.group(1))
                    evicted_blocks = int(evicted_match.group(1))
                    
                    return write_size, compacted_blocks, evicted_blocks
                    
    except FileNotFoundError:
        print(f"Warning: {filename} not found")
        return None, None, None
    
    return None, None, None

# Get final stats for all policies
policy_stats = {}
min_write_size = float('inf')

print("Final statistics for each policy:")
for filename, label in zip(files, labels):
    write_size, compacted, evicted = get_final_stats(filename)
    if write_size is not None:
        policy_stats[label] = {
            'write_size': write_size,
            'compacted_blocks': compacted,
            'evicted_blocks': evicted
        }
        min_write_size = min(min_write_size, write_size)
        print(f"{label}: write_size={write_size}, compacted={compacted}, evicted={evicted}")

print(f"\nMinimum write_size across all policies: {min_write_size}")

# Calculate CO2 values at the minimum write size point
print(f"\nCO2 calculations at write_size = {min_write_size}:")
co2_values = {}

for label in policy_stats:
    stats = policy_stats[label]
    # Formula: compacted_blocks + evicted_blocks * 2.37 + write_size_to_cache / 4096
    co2_value = stats['compacted_blocks'] + stats['evicted_blocks'] * 2.37 + min_write_size / 4096
    co2_values[label] = co2_value
    print(f"{label}: {co2_value:.2f}")

# DP Optimizer calculation
# Need to find the corresponding timestep for min_write_size
print(f"\nFinding DP optimizer value at equivalent write size...")

# Read DP data
df_dp = pd.read_csv('fresh_dp_clean.csv', header=None, names=['time', 'chosen_c', 'base_F_t', 'trans_penalty', 'cumulative_cost'])

# Convert DP timesteps to written bytes (32MB per timestep)
dp_written_bytes = df_dp['time'] * 32 * 1024 * 1024

# Find the closest timestep to min_write_size
closest_idx = (dp_written_bytes - min_write_size).abs().idxmin()
closest_timestep = df_dp.iloc[closest_idx]['time']
closest_cumulative_cost = df_dp.iloc[closest_idx]['cumulative_cost']

print(f"Closest DP timestep: {closest_timestep} (write_size: {dp_written_bytes.iloc[closest_idx]})")
print(f"DP cumulative cost at this point: {closest_cumulative_cost}")

# DP CO2 calculation: cumulative_cost + T * 32MB/4k
dp_co2 = closest_cumulative_cost + closest_timestep * 32 * 1024 * 1024 / 4096
co2_values['DP_Optimizer'] = dp_co2
print(f"DP Optimizer CO2: {dp_co2:.2f}")

# Normalize to FIFO
print(f"\nNormalized CO2 values (FIFO = 1.0):")
fifo_co2 = co2_values['FIFO']
for label, co2_value in co2_values.items():
    normalized = co2_value / fifo_co2
    print(f"{label}: {normalized:.3f}")