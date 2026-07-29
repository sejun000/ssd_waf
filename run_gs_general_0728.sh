#!/bin/bash
# General GS_FINAL (REFLASH) re-run on the 3 alibaba traces at r=8.64.
# Same config as run_gs_clean_3traces.sh, but tags carry the 0728 date so
# earlier GS_FINAL_clean_* results are not clobbered.
set -u
POLICY=LOG_GREEDY_COST_BENEFIT_10_GS_FINAL
MA=ewma
MAW=1572864
DEV=15000000000000
COLD=16050000000000
ALIGN=13079937024
CACHE=$(( ((DEV / 8 + ALIGN - 1) / ALIGN) * ALIGN ))
SCALE=2
R=8.64
RT=864
D=1

cd "$(dirname "$0")"

run_one() {
    local TRACE_NAME=$1
    local TRACE=$2
    local TS=$(date +%y%m%d_%H%M%S)
    local TAG="GS_FINAL_0728_${TRACE_NAME}"
    local STAT="${TAG}.stat_pr${RT}"
    local WAF="${TAG}_pr${RT}_${TS}.waf.log"
    local LOG="run_${TAG}_pr${RT}.log"
    local GSDEC="${TAG}_pr${RT}.gsdec.log"
    echo "[start ${TRACE_NAME}] gsdec=$GSDEC  trace=$TRACE"
    GS_DECISION_LOG="$GSDEC" \
    ./cache_sim "$TRACE" "$CACHE" \
        --rw_policy write-only --trace_format csv \
        --cache_policy "$POLICY" \
        --cold_capacity "$COLD" \
        --waf_log_file "$WAF" \
        --periodic_ratio "$R" --util_step 0.02 \
        --moving_avg_type "$MA" --moving_avg_window "$MAW" \
        --gs_decision_period_segs "$D" \
        --stat_log_file "$STAT" --scale "$SCALE" \
        > "$LOG" 2>&1 &
    echo "  pid=$!"
    sleep 1
}

run_one dwpd01to1 /home/sejun000/alibaba_dwpd01to1.trace
run_one dwpd1to2  /home/sejun000/alibaba_dwpd1to2_4x.trace
run_one dwpd2_5x  /home/sejun000/alibaba_dwpd2_5x.trace

echo "all 3 launched. waiting..."
wait
echo "all done at $(date)"
