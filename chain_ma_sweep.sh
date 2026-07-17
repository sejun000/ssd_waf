#!/bin/bash
# 3 policies × 2 us × 2 MA types = 12 sub-sweeps × 5 r = 60 runs.
# segment_size_blocks = 6GB/4KB = 1572864. SMA window = ×32 = 50331648 blocks.
cd "$(dirname "$0")"
SMA_WIN=50331648
EWMA_WIN=0   # 0 → LogCache uses DEFAULT_HALF_LIFE_IN_BLOCKS

echo "[chain_ma] start at $(date)"
for POLICY in LOG_GREEDY_COST_BENEFIT_10 LOG_GREEDY_COST_BENEFIT_10_TDELTA LOG_GREEDY_COST_BENEFIT_10_GC; do
    for US in 0.02 0.10; do
        for MA in ewma sma; do
            if [ "$MA" = "ewma" ]; then WIN=$EWMA_WIN; else WIN=$SMA_WIN; fi
            echo "[chain_ma] launching ${POLICY} us=${US} ma=${MA} win=${WIN} at $(date)"
            ./run_ma_sweep.sh "$POLICY" "$US" "$MA" "$WIN" > "sweep_${POLICY}_us${US}_${MA}.log" 2>&1
            echo "[chain_ma] ${POLICY} us=${US} ma=${MA} done at $(date)"
        done
    done
done
echo "[chain_ma] all done at $(date)"
