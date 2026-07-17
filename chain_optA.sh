#!/bin/bash
# Option A: Re-run GC + GC_NAND x {us=0.02, 0.05, 0.10}, 5 concurrent each.
# Sequential 6 sub-sweeps × 5 r-points = 30 runs total.
cd "$(dirname "$0")"
echo "[chain_optA] start at $(date)"
for POLICY in LOG_GREEDY_COST_BENEFIT_10_GC LOG_GREEDY_COST_BENEFIT_10_GC_NAND; do
    for US in 0.02 0.05 0.10; do
        echo "[chain_optA] launching ${POLICY} us=${US} at $(date)"
        ./run_us_sweep.sh "${POLICY}" "${US}" > "sweep_${POLICY}_us${US}.log" 2>&1
        echo "[chain_optA] ${POLICY} us=${US} done at $(date)"
    done
done
echo "[chain_optA] all done at $(date)"
