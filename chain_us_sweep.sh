#!/bin/bash
# Run 4 sweeps SEQUENTIALLY: {GD, GC} x {us=0.02, us=0.05}, 5 concurrent each.
cd "$(dirname "$0")"
echo "[chain_us] start at $(date)"
for US in 0.02 0.05; do
    for POLICY in LOG_GREEDY_COST_BENEFIT_10 LOG_GREEDY_COST_BENEFIT_10_GC; do
        echo "[chain_us] launching ${POLICY} us=${US} at $(date)"
        ./run_us_sweep.sh "${POLICY}" "${US}" > "sweep_${POLICY}_us${US}.log" 2>&1
        echo "[chain_us] ${POLICY} us=${US} done at $(date)"
    done
done
echo "[chain_us] all done at $(date)"
