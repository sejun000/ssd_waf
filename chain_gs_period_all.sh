#!/bin/bash
# Full GS sweep: segs={1,2,4,8,16,32,64,128}, 5 r each.
# Sequential batches (5 parallel inside). Expected ~30 min/batch -> ~4 hours.
cd "$(dirname "$0")"

echo "[chain_gs_period_all] start at $(date)"
for P in 1 2 4 8 16 32 64 128; do
    echo "[chain_gs_period_all] launching segs=${P} at $(date)"
    ./run_gs_period_sweep.sh "$P" > "sweep_GS_segs${P}.log" 2>&1
    echo "[chain_gs_period_all] segs=${P} done at $(date)"
done
echo "[chain_gs_period_all] all done at $(date)"
