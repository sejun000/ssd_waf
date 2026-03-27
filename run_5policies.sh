#!/bin/bash
# Run 5 GC policies in parallel
# Usage: ./run_5policies.sh [trace_file]
#   default trace: /mnt/ramdisk/alibaba_dwpd2_5x.trace

TRACE=${1:-/home/sejun000/alibaba_dwpd01to1.trace}

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

MAX_PARALLEL=3
RUNNING_PIDS=()
RUNNING_NAMES=()

for POLICY in "${POLICIES[@]}"; do
    # Wait if already MAX_PARALLEL running
    while [ ${#RUNNING_PIDS[@]} -ge $MAX_PARALLEL ]; do
        # Wait for any one to finish
        wait -n -p DONE_PID "${RUNNING_PIDS[@]}" 2>/dev/null
        EXIT=$?
        # Find and remove finished pid
        NEW_PIDS=()
        NEW_NAMES=()
        for i in "${!RUNNING_PIDS[@]}"; do
            if kill -0 "${RUNNING_PIDS[$i]}" 2>/dev/null; then
                NEW_PIDS+=("${RUNNING_PIDS[$i]}")
                NEW_NAMES+=("${RUNNING_NAMES[$i]}")
            else
                echo "${RUNNING_NAMES[$i]} done (exit=$EXIT)"
            fi
        done
        RUNNING_PIDS=("${NEW_PIDS[@]}")
        RUNNING_NAMES=("${NEW_NAMES[@]}")
    done

    echo "Starting $POLICY ..."
    python3 ./blk_trace_analysis.py "$TRACE" \
        --cache_policy "$POLICY" \
        --trace_format csv \
        --rw_policy write-only \
        --scale 2 \
        > "run_${POLICY}.log" 2>&1 &
    RUNNING_PIDS+=($!)
    RUNNING_NAMES+=("$POLICY")
done

echo "---"
echo "Waiting for remaining ${#RUNNING_PIDS[@]} to finish..."

for i in "${!RUNNING_PIDS[@]}"; do
    wait "${RUNNING_PIDS[$i]}"
    echo "${RUNNING_NAMES[$i]} done (exit=$?)"
done

echo "All done at $(date '+%Y%m%d_%H%M%S')"
