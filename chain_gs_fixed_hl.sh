#!/bin/bash
# Sequential chain: r=8.64 batch (5 in parallel, hl×4) → wait → r=2.88 batch.
set -u
cd "$(dirname "$0")"
MAW=6291456    # = 1572864 * 4
WAIT_SUFFIX1="gsdec864hl${MAW}fix"
WAIT_SUFFIX2="gsdec288hl${MAW}fix"

echo "$(date) === batch 1: r=8.64 hl=$MAW ==="
bash run_gs_fixed_hl.sh 8.64 $MAW
sleep 5
while pgrep -f "cache_sim.*$WAIT_SUFFIX1" >/dev/null; do
    sleep 60
done
echo "$(date) === batch 1 done ==="

echo "$(date) === batch 2: r=2.88 hl=$MAW ==="
bash run_gs_fixed_hl.sh 2.88 $MAW
sleep 5
while pgrep -f "cache_sim.*$WAIT_SUFFIX2" >/dev/null; do
    sleep 60
done
echo "$(date) === ALL DONE ==="
