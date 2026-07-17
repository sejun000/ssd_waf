#!/bin/bash
# Post-fix chain: 3 policies × us=0.02 × {ewma, sma} = 6 sub-sweeps × 5 r = 30 runs.
# Uses run_ma_sweep_fix.sh which appends "_fix" to tags so output stays separated
# from the pre-fix chain that may still be running.
cd "$(dirname "$0")"
SMA_WIN=50331648
EWMA_WIN=0   # 0 → LogCache uses DEFAULT_HALF_LIFE_IN_BLOCKS

echo "[chain_ma_fix] start at $(date)"
for POLICY in LOG_GREEDY_COST_BENEFIT_10 LOG_GREEDY_COST_BENEFIT_10_TDELTA LOG_GREEDY_COST_BENEFIT_10_GC; do
    for MA in ewma sma; do
        if [ "$MA" = "ewma" ]; then WIN=$EWMA_WIN; else WIN=$SMA_WIN; fi
        echo "[chain_ma_fix] launching ${POLICY} us=0.02 ma=${MA} win=${WIN} at $(date)"
        ./run_ma_sweep_fix.sh "$POLICY" "0.02" "$MA" "$WIN" > "sweep_${POLICY}_us0.02_${MA}_fix.log" 2>&1
        echo "[chain_ma_fix] ${POLICY} us=0.02 ma=${MA} done at $(date)"
    done
done
echo "[chain_ma_fix] all done at $(date)"
