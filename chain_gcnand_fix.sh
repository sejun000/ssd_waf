#!/bin/bash
# Re-run GC_NAND x {us=0.02, 0.05, 0.10} after current_waf fix.
cd "$(dirname "$0")"
echo "[chain_gcnand_fix] start at $(date)"
for US in 0.02 0.05 0.10; do
    POLICY=LOG_GREEDY_COST_BENEFIT_10_GC_NAND
    echo "[chain_gcnand_fix] launching ${POLICY} us=${US} at $(date)"
    ./run_us_sweep.sh "${POLICY}" "${US}" > "sweep_${POLICY}_us${US}.log" 2>&1
    echo "[chain_gcnand_fix] ${POLICY} us=${US} done at $(date)"
done
echo "[chain_gcnand_fix] all done at $(date)"
