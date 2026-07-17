#!/bin/bash
# Wait for the currently-running r=8.64 batch to finish, then launch r=2.88 batch.
# Uses the SUFFIX (gsdec<RT>fix) to robustly identify batch processes.
set -u
cd "$(dirname "$0")"

echo "$(date) === waiting for r=8.64 batch (suffix gsdec864fix) ==="
while pgrep -f "cache_sim.*gsdec864fix" >/dev/null; do
    sleep 60
done
echo "$(date) === r=8.64 done, launching r=2.88 ==="
bash run_gs_fixed_all.sh 2.88
sleep 5
while pgrep -f "cache_sim.*gsdec288fix" >/dev/null; do
    sleep 60
done
echo "$(date) === ALL DONE ==="
