#!/bin/bash
# Chain GC EWMA sweep across half-lives (excluding 1x baseline which is already done).
# Half-life multipliers vs DEFAULT_HALF_LIFE_IN_BLOCKS=6291456:
#   0.5x -> 3145728   (faster)
#   2x   -> 12582912
#   4x   -> 25165824
#   8x   -> 50331648  (matches SMA window from prior chain)
# Each: 5 runs in parallel; 4 sub-sweeps run sequentially. Total 20 runs.
cd "$(dirname "$0")"
POLICY=LOG_GREEDY_COST_BENEFIT_10_GC
US=0.02

echo "[chain_ewma_hl] start at $(date)"
for HL in 3145728 12582912 25165824 50331648; do
    echo "[chain_ewma_hl] launching HL=${HL} at $(date)"
    ./run_ewma_halflife_sweep.sh "$POLICY" "$US" "$HL" \
        > "sweep_${POLICY}_us02_ewma_hl${HL}.log" 2>&1
    echo "[chain_ewma_hl] HL=${HL} done at $(date)"
done
echo "[chain_ewma_hl] all done at $(date)"
