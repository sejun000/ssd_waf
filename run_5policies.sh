#!/bin/bash
# Run 5 GC policies in parallel
# Usage: ./run_5policies.sh [trace_file]
#   default trace: /mnt/ramdisk/alibaba_dwpd2_5x.trace

TRACE=${1:-/mnt/nvme0/alibaba_dwpd01to1.trace}

POLICIES=(
    "LOG_SEPBIT_FIFO"
    "LOG_GREEDY_80"
    "LOG_GREEDY_COST_BENEFIT_80"
    "LOG_GREEDY_COST_BENEFIT_80_COLD"
    "MIDAS_CACHE"
)

echo "Trace: $TRACE"
echo "Policies: ${POLICIES[*]}"
echo "Starting at $(date '+%Y%m%d_%H%M%S')"
echo "---"

PIDS=()
for POLICY in "${POLICIES[@]}"; do
    echo "Starting $POLICY ..."
    python3 ./blk_trace_analysis.py "$TRACE" \
        --cache_policy "$POLICY" \
        --trace_format csv \
        --rw_policy write-only \
        --scale 2 \
        > "run_${POLICY}.log" 2>&1 &
    PIDS+=($!)
done

echo "---"
echo "PIDs: ${PIDS[*]}"
echo "Waiting for all to finish..."

for i in "${!PIDS[@]}"; do
    wait "${PIDS[$i]}"
    echo "${POLICIES[$i]} done (exit=$?)"
done

echo "All done at $(date '+%Y%m%d_%H%M%S')"
