#!/bin/bash
# Run 3 sweeps SEQUENTIALLY: GC_NAND x {us=0.02, 0.05, 0.10}, 5 concurrent each.
cd "$(dirname "$0")"
echo "[chain_gcnand] start at $(date)"
for US in 0.02 0.05 0.10; do
    POLICY=LOG_GREEDY_COST_BENEFIT_10_GC_NAND
    echo "[chain_gcnand] launching ${POLICY} us=${US} at $(date)"
    ./run_us_sweep.sh "${POLICY}" "${US}" > "sweep_${POLICY}_us${US}.log" 2>&1
    echo "[chain_gcnand] ${POLICY} us=${US} done at $(date)"
done
echo "[chain_gcnand] all done at $(date)"
