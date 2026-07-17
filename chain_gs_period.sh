#!/bin/bash
# Chain GS sweeps across decision-period values {1,2,4,8,16,32}.
# Each sweep is 5-parallel (r in {2,4,6,8,10}) ≈ 30 min wall.
# Total: 6 sweeps × ~30 min ≈ 3 hours.
cd "$(dirname "$0")"

echo "[chain_gs_period] start at $(date)"
for P in 1 2 4 8 16 32; do
    echo "[chain_gs_period] launching segs=${P} at $(date)"
    ./run_gs_period_sweep.sh "$P" > "sweep_GS_segs${P}.log" 2>&1
    echo "[chain_gs_period] segs=${P} done at $(date)"
done
echo "[chain_gs_period] all done at $(date)"
