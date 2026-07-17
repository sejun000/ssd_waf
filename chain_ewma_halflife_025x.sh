#!/bin/bash
# Wait for the running half-life chain (pid 901413) to finish, then run 0.25x sub-sweep.
# 0.25x = DEFAULT_HALF_LIFE_IN_BLOCKS / 4 = 1572864
cd "$(dirname "$0")"
WAIT_PID=901413
echo "[chain_ewma_hl_025x] waiting on pid ${WAIT_PID} at $(date)"
while kill -0 "$WAIT_PID" 2>/dev/null; do
    sleep 60
done
echo "[chain_ewma_hl_025x] pid ${WAIT_PID} gone, launching 0.25x at $(date)"
./run_ewma_halflife_sweep.sh "LOG_GREEDY_COST_BENEFIT_10_GC" "0.02" "1572864" \
    > "sweep_LOG_GREEDY_COST_BENEFIT_10_GC_us02_ewma_hl1572864.log" 2>&1
echo "[chain_ewma_hl_025x] all done at $(date)"
