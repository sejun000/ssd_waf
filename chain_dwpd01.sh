#!/bin/bash
# 4 policies sequentially on dwpd01to1 trace.
# GD/TDELTA/GC: ewma window=0 (default 6.29M).
# GS:           ewma window=3145728 (default x 0.5, best for GS).
cd "$(dirname "$0")"

echo "[chain_dwpd01] start at $(date)"
for POL in LOG_GREEDY_COST_BENEFIT_10 \
           LOG_GREEDY_COST_BENEFIT_10_TDELTA \
           LOG_GREEDY_COST_BENEFIT_10_GC; do
    echo "[chain_dwpd01] launching ${POL} at $(date)"
    ./run_ma_sweep_dwpd01.sh "$POL" 0.02 ewma 0 > "sweep_${POL}_dwpd01.log" 2>&1
    echo "[chain_dwpd01] ${POL} done at $(date)"
done

# GS uses hl=3.15M (best)
echo "[chain_dwpd01] launching LOG_GREEDY_COST_BENEFIT_10_GS hl=3.15M at $(date)"
./run_ma_sweep_dwpd01.sh LOG_GREEDY_COST_BENEFIT_10_GS 0.02 ewma 3145728 > sweep_GS_dwpd01.log 2>&1
echo "[chain_dwpd01] GS done at $(date)"

echo "[chain_dwpd01] all done at $(date)"
