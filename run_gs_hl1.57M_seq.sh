#!/bin/bash
# Sequential 5-r sweep (one cache_sim at a time) for GS at hl=1.57M.
# Used when memory pressure forbids parallel (dwpd01 chain occupying ~640GB).
set -u
POLICY=LOG_GREEDY_COST_BENEFIT_10_GS
US=0.02
US_TAG=02
MA_TYPE=ewma
MA_WINDOW=1572864
TAG="${POLICY}_us${US_TAG}_${MA_TYPE}_thetaI_hl${MA_WINDOW}"

TRACE=/home/sejun000/alibaba_dwpd1to2_4x.trace
DEVICE_SIZE=15000000000000
COLD_CAP=16050000000000
ALIGN=13079937024
CACHE=$(( ((DEVICE_SIZE / 8 + ALIGN - 1) / ALIGN) * ALIGN ))
SCALE=2

cd "$(dirname "$0")"
TS=$(date +%y%m%d_%H%M%S)
echo "[seq] tag=$TAG cache=$CACHE ts=$TS"
for r in 2 4 6 8 10; do
    STAT="${TAG}.stat_pr${r}"
    WAF="${TAG}_pr${r}_${TS}.waf.log"
    LOG="run_${TAG}_pr${r}.log"
    echo "[seq] r=$r start at $(date)"
    ./cache_sim "$TRACE" "$CACHE" \
        --rw_policy write-only \
        --trace_format csv \
        --cache_policy "$POLICY" \
        --cache_trace /mnt/nvme2n2/${TAG}_pr${r}.trace \
        --cold_trace /mnt/nvme2n2/${TAG}_pr${r}.cold.trace \
        --cold_capacity "$COLD_CAP" \
        --waf_log_file "$WAF" \
        --periodic_ratio "$r" \
        --util_step "$US" \
        --moving_avg_type "$MA_TYPE" \
        --moving_avg_window "$MA_WINDOW" \
        --stat_log_file "$STAT" \
        --scale "$SCALE" \
        > "$LOG" 2>&1
    echo "[seq] r=$r done at $(date)"
done
echo "[seq] all done at $(date)"
