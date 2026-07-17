#!/bin/bash
# Chain: r=8.64 batch (6 segs in parallel, per-seg hl) → wait → r=2.88 batch.
set -u
cd "$(dirname "$0")"

echo "$(date) === batch 1: r=8.64 tickfix (per-seg hl=2*P) ==="
bash run_gs_tickfix_all.sh 8.64
sleep 5
while pgrep -f "cache_sim.*gsdec864hl2xseg" >/dev/null; do
    sleep 60
done
echo "$(date) === batch 1 done ==="

echo "$(date) === batch 2: r=2.88 tickfix (per-seg hl=2*P) ==="
bash run_gs_tickfix_all.sh 2.88
sleep 5
while pgrep -f "cache_sim.*gsdec288hl2xseg" >/dev/null; do
    sleep 60
done
echo "$(date) === ALL DONE ==="
