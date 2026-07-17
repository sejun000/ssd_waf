#!/bin/bash
# Run LOG_GREEDY_COST_BENEFIT_10 with different periodic_ratio values in parallel
TRACE=/home/sejun000/alibaba_dwpd1to2_4x.trace
POLICY=LOG_GREEDY_COST_BENEFIT_10
FORMAT=csv
RW=write-only
SCALE=2

for ratio in 2 4 6 8 10; do
    echo "Starting periodic_ratio=${ratio}..."
    python3 ./blk_trace_analysis.py "$TRACE" \
        --cache_policy "$POLICY" \
        --trace_format "$FORMAT" \
        --rw_policy "$RW" \
        --scale "$SCALE" \
        --periodic_ratio "$ratio" \
        > "run_periodic_r${ratio}.log" 2>&1 &
done

echo "All 5 jobs launched. PIDs:"
jobs -p
echo "Logs: run_periodic_r{2,4,6,8,10}.log"
wait
echo "All done."
