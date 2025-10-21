#!/usr/bin/env python3
import matplotlib.pyplot as plt
import pandas as pd
import numpy as np

df = pd.read_csv('constrained_dp_clean.csv', header=None, names=['time', 'chosen_c', 'base_F_t', 'trans_penalty', 'cumulative_cost'])

optimal_cost_data = """0.400000, 1.54515e+09
0.410000, 1.54129e+09
0.420000, 1.53736e+09
0.430000, 1.5335e+09
0.440000, 1.52959e+09
0.450000, 1.52574e+09
0.460000, 1.52183e+09
0.470000, 1.51802e+09
0.480000, 1.51421e+09
0.490000, 1.51061e+09
0.500000, 1.5069e+09
0.510000, 1.50291e+09
0.520000, 1.49893e+09
0.530000, 1.49493e+09
0.540000, 1.49095e+09
0.550000, 1.48699e+09
0.560000, 1.48304e+09
0.570000, 1.47907e+09
0.580000, 1.47512e+09
0.590000, 1.47117e+09
0.600000, 1.46718e+09
0.610000, 1.46322e+09
0.620000, 1.45936e+09
0.630000, 1.45597e+09
0.640000, 1.45269e+09
0.650000, 1.44869e+09
0.660000, 1.4447e+09
0.670000, 1.44077e+09
0.680000, 1.43697e+09
0.690000, 1.43341e+09
0.700000, 1.42979e+09
0.710000, 1.4276e+09
0.720000, 1.42431e+09
0.730000, 1.42268e+09
0.740000, 1.42078e+09
0.750000, 1.4202e+09
0.760000, 1.42016e+09
0.770000, 1.42099e+09
0.780000, 1.42131e+09
0.790000, 1.42215e+09
0.800000, 1.42342e+09
0.810000, 1.42507e+09
0.820000, 1.42671e+09
0.830000, 1.42743e+09
0.840000, 1.42731e+09
0.850000, 1.42897e+09
0.860000, 1.43005e+09
0.870000, 1.43085e+09
0.880000, 1.43233e+09
0.890000, 1.43392e+09
0.900000, 1.4356e+09
0.910000, 1.43728e+09
0.920000, 1.439e+09
0.930000, 1.44062e+09"""

ratios = []
costs = []
for line in optimal_cost_data.strip().split('\n'):
    ratio, cost = line.split(', ')
    ratios.append(float(ratio))
    costs.append(float(cost))

optimal_df = pd.DataFrame({'ratio': ratios, 'cost': costs})

fig, ((ax1, ax2), (ax3, ax4)) = plt.subplots(2, 2, figsize=(15, 12))

# 1. 시간에 따른 최적 선택 (상위 10%)
sample_indices = np.linspace(0, len(df)-1, len(df)//10, dtype=int)
sampled_df = df.iloc[sample_indices]

ax1.plot(sampled_df['time'], sampled_df['chosen_c'], 'b-', alpha=0.7, linewidth=1.5)
ax1.set_title('DP Optimizer의 시간에 따른 최적 Valid Ratio 선택 (샘플링)', fontsize=12)
ax1.set_xlabel('Time Step')
ax1.set_ylabel('Chosen Valid Ratio')
ax1.grid(True, alpha=0.3)
ax1.set_ylim(0.4, 1.0)

# 2. Valid Ratio별 최종 비용 분포
ax2.plot(optimal_df['ratio'], optimal_df['cost'] / 1e9, 'ro-', markersize=4, linewidth=2)
min_idx = optimal_df['cost'].idxmin()
ax2.plot(optimal_df.iloc[min_idx]['ratio'], optimal_df.iloc[min_idx]['cost'] / 1e9, 'go', markersize=10, label=f'최적 c=0.76')
ax2.set_title('Valid Ratio별 최종 총 비용', fontsize=12)
ax2.set_xlabel('Valid Ratio')
ax2.set_ylabel('총 비용 (Billions)')
ax2.grid(True, alpha=0.3)
ax2.legend()

# 3. 누적 비용 증가 (상위 5%)
sample_indices = np.linspace(0, len(df)-1, len(df)//20, dtype=int)
sampled_df = df.iloc[sample_indices]

ax3.plot(sampled_df['time'], sampled_df['cumulative_cost'] / 1e9, 'g-', linewidth=2)
ax3.set_title('시간에 따른 누적 비용 증가', fontsize=12)
ax3.set_xlabel('Time Step')
ax3.set_ylabel('누적 비용 (Billions)')
ax3.grid(True, alpha=0.3)

# 4. Valid Ratio 분포 히스토그램
valid_ratios = df['chosen_c'].values
unique_ratios, counts = np.unique(valid_ratios, return_counts=True)

ax4.bar(unique_ratios, counts, alpha=0.7, color='skyblue', edgecolor='navy', width=0.005)
ax4.axvline(x=0.76, color='red', linestyle='--', linewidth=2, label='최적값 c=0.76')
ax4.set_title('최적 경로에서의 Valid Ratio 선택 빈도', fontsize=12)
ax4.set_xlabel('Valid Ratio')
ax4.set_ylabel('선택 횟수')
ax4.grid(True, alpha=0.3)
ax4.legend()

plt.tight_layout()
plt.savefig('dp_optimizer_analysis.png', dpi=300, bbox_inches='tight')
print("그래프가 dp_optimizer_analysis.png로 저장되었습니다.")

print(f"\n=== DP Optimizer 최적 경로 분석 ===")
print(f"전체 시간 스텝: {len(df):,}")
print(f"최적 Valid Ratio: 0.76")
print(f"최종 총 비용: {optimal_df.iloc[min_idx]['cost']:,.0f}")
print(f"가장 많이 선택된 Valid Ratio: {unique_ratios[np.argmax(counts)]:.2f} ({np.max(counts):,}회)")

ratio_changes = (df['chosen_c'].diff() != 0).sum()
print(f"Valid Ratio 변경 횟수: {ratio_changes:,}")