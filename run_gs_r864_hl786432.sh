#!/bin/bash
# GS v4 EWMA hl=786432 (절반), r=8.64, segs 1~16 all parallel.
set -u
POLICY=LOG_GREEDY_COST_BENEFIT_10_GS
MA_TYPE=ewma
MA_WINDOW=786432
TRACE=/home/sejun000/alibaba_dwpd1to2_4x.trace
DEVICE_SIZE=15000000000000
COLD_CAP=16050000000000
ALIGN=13079937024
CACHE=$(( ((DEVICE_SIZE / 8 + ALIGN - 1) / ALIGN) * ALIGN ))
SCALE=2
R=8.64
R_TAG=864

cd "$(dirname "$0")"
TS=$(date +%y%m%d_%H%M%S)
echo "GS v4 EWMA hl=${MA_WINDOW} r=${R} sweep ts=${TS}"

for P in 1 2 4 8 16; do
    TAG="${POLICY}_us02_${MA_TYPE}_hl${MA_WINDOW}_gsv4_segs${P}"
    STAT="${TAG}.stat_pr${R_TAG}"
    WAF="${TAG}_pr${R_TAG}_${TS}.waf.log"
    LOG="run_${TAG}_pr${R_TAG}.log"
    echo "  segs=${P} -> ${STAT}"
    nohup ./cache_sim "$TRACE" "$CACHE" \
        --rw_policy write-only \
        --trace_format csv \
        --cache_policy "$POLICY" \
        --cache_trace /mnt/nvme2n2/${TAG}_pr${R_TAG}.trace \
        --cold_trace /mnt/nvme2n2/${TAG}_pr${R_TAG}.cold.trace \
        --cold_capacity "$COLD_CAP" \
        --waf_log_file "$WAF" \
        --periodic_ratio "$R" \
        --util_step 0.02 \
        --moving_avg_type "$MA_TYPE" \
        --moving_avg_window "$MA_WINDOW" \
        --gs_decision_period_segs "$P" \
        --stat_log_file "$STAT" \
        --scale "$SCALE" \
        > "$LOG" 2>&1 &
    echo "    pid=$!"
done
wait
echo "done at $(date)"
