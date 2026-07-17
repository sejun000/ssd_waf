#!/bin/bash
# GS v4 + SMA window = 128 segments (= 201,326,592 blocks).
# r=8 sweep, two batches: small {1,2,4,8} first, then large {16,32,64,128}.
set -u
POLICY=LOG_GREEDY_COST_BENEFIT_10_GS
MA_TYPE=sma
MA_WINDOW=201326592   # 128 segments * 1572864 blocks/segment
TRACE=/home/sejun000/alibaba_dwpd1to2_4x.trace
DEVICE_SIZE=15000000000000
COLD_CAP=16050000000000
ALIGN=13079937024
CACHE=$(( ((DEVICE_SIZE / 8 + ALIGN - 1) / ALIGN) * ALIGN ))
SCALE=2
R=8.64
R_TAG=864   # pr${R_TAG} suffix in output filenames

cd "$(dirname "$0")"
TS=$(date +%y%m%d_%H%M%S)
echo "GS v4 SMA w=${MA_WINDOW} r=${R} batched sweep, ts=${TS}"

launch_one() {
    local P=$1
    local TAG="${POLICY}_us02_${MA_TYPE}_w${MA_WINDOW}_gsv4_segs${P}"
    local STAT="${TAG}.stat_pr${R_TAG}"
    local WAF="${TAG}_pr${R_TAG}_${TS}.waf.log"
    local LOG="run_${TAG}_pr${R_TAG}.log"
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
}

echo "[batch 1] small segs {1,2,4,8} at $(date)"
for P in 1 2 4 8; do launch_one "$P"; done
wait
echo "[batch 1] done at $(date)"

echo "[batch 2] large segs {16,32,64,128} at $(date)"
for P in 16 32 64 128; do launch_one "$P"; done
wait
echo "[batch 2] done at $(date)"

echo "GS v4 SMA r=${R} batched sweep all done at $(date)"
