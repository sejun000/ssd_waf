#!/bin/bash
# Extend GS sweep to segs={64,128}. Sequential (5 parallel inside each).
# Expected ~30 min per batch -> ~1 hour total.
cd "$(dirname "$0")"

echo "[chain_gs_period_64_128] start at $(date)"
for P in 64 128; do
    echo "[chain_gs_period_64_128] launching segs=${P} at $(date)"
    ./run_gs_period_sweep.sh "$P" > "sweep_GS_segs${P}.log" 2>&1
    echo "[chain_gs_period_64_128] segs=${P} done at $(date)"
done
echo "[chain_gs_period_64_128] all done at $(date)"
