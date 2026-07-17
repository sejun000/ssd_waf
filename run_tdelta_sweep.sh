#!/bin/bash
# Sweep r in {2,4,6,8,10} for one policy, in parallel.
# Usage: run_tdelta_sweep.sh <POLICY>
set -u
POLICY="${1:-LOG_GREEDY_COST_BENEFIT_10}"

TRACE=/home/sejun000/alibaba_dwpd1to2_4x.trace
DEVICE_SIZE=15000000000000
COLD_CAP=16050000000000          # device_size * 1.07
ALIGN=13079937024
CACHE=$(( ((DEVICE_SIZE / 8 + ALIGN - 1) / ALIGN) * ALIGN ))   # ratio = 0.125
SCALE=2

cd "$(dirname "$0")"
TS=$(date +%y%m%d_%H%M%S)

echo "Policy=${POLICY}  cache=${CACHE}  cold=${COLD_CAP}  ts=${TS}"
for r in 2 4 6 8 10; do
    STAT="${POLICY}.stat_pr${r}"
    WAF="${POLICY}_pr${r}_${TS}.waf.log"
    LOG="run_${POLICY}_pr${r}.log"
    echo "Launching r=${r} -> stat=${STAT} waf=${WAF}"
    nohup ./cache_sim "$TRACE" "$CACHE" \
        --rw_policy write-only \
        --trace_format csv \
        --cache_policy "$POLICY" \
        --cache_trace /mnt/nvme2n2/${POLICY}_pr${r}.trace \
        --cold_trace /mnt/nvme2n2/${POLICY}_pr${r}.cold.trace \
        --cold_capacity "$COLD_CAP" \
        --waf_log_file "$WAF" \
        --periodic_ratio "$r" \
        --stat_log_file "$STAT" \
        --scale "$SCALE" \
        > "$LOG" 2>&1 &
    echo "  pid=$!"
done
echo "All 5 ${POLICY} launched. Waiting for completion..."
wait
echo "${POLICY} sweep done at $(date)"
