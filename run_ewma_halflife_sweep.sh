#!/bin/bash
# Sweep r in {2,4,6,8,10} for one (policy, util_step, EWMA half-life).
# Tag: <POLICY>_us<US>_ewma_hl<HL>  — half-life encoded so files don't collide.
# Usage: run_ewma_halflife_sweep.sh <POLICY> <UTIL_STEP> <HALFLIFE_BLOCKS>
set -u
POLICY="${1:-LOG_GREEDY_COST_BENEFIT_10_GC}"
US="${2:-0.02}"
HL="${3:-6291456}"
US_TAG=$(echo "$US" | sed 's/0\.//; s/\.//')

TRACE=/home/sejun000/alibaba_dwpd1to2_4x.trace
DEVICE_SIZE=15000000000000
COLD_CAP=16050000000000
ALIGN=13079937024
CACHE=$(( ((DEVICE_SIZE / 8 + ALIGN - 1) / ALIGN) * ALIGN ))
SCALE=2

cd "$(dirname "$0")"
TS=$(date +%y%m%d_%H%M%S)
TAG="${POLICY}_us${US_TAG}_ewma_hl${HL}"

echo "Policy=${POLICY}  util_step=${US}  ma=ewma half-life=${HL}  tag=${TAG}  cache=${CACHE}  ts=${TS}"
for r in 2 4 6 8 10; do
    STAT="${TAG}.stat_pr${r}"
    WAF="${TAG}_pr${r}_${TS}.waf.log"
    LOG="run_${TAG}_pr${r}.log"
    echo "Launching r=${r} -> stat=${STAT}"
    nohup ./cache_sim "$TRACE" "$CACHE" \
        --rw_policy write-only \
        --trace_format csv \
        --cache_policy "$POLICY" \
        --cache_trace /mnt/nvme2n2/${TAG}_pr${r}.trace \
        --cold_trace /mnt/nvme2n2/${TAG}_pr${r}.cold.trace \
        --cold_capacity "$COLD_CAP" \
        --waf_log_file "$WAF" \
        --periodic_ratio "$r" \
        --util_step "$US" \
        --moving_avg_type "ewma" \
        --moving_avg_window "$HL" \
        --stat_log_file "$STAT" \
        --scale "$SCALE" \
        > "$LOG" 2>&1 &
    echo "  pid=$!"
done
echo "All 5 ${TAG} launched. Waiting..."
wait
echo "${TAG} sweep done at $(date)"
