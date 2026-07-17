#!/bin/bash
# RESORT_CORRECT=1 on the remaining 2 traces (dwpd01to1, dwpd1to2_4x).
# Mirrors run_reflash_3traces.sh / run_resort_test.sh config.
set -u
POLICY=LOG_GREEDY_COST_BENEFIT_10_GS_FINAL
MA=ewma
MAW=1572864
DEV=15000000000000
COLD=16050000000000
ALIGN=13079937024
CACHE=$(( ((DEV / 8 + ALIGN - 1) / ALIGN) * ALIGN ))
D=1
R=8.64
RT=864

cd "$(dirname "$0")"

run_one() {
    local TAGSHORT=$1
    local TRACE=$2
    local SCALE=$3
    local TS=$(date +%y%m%d_%H%M%S)
    local TAG="GS_FINAL_resortcorr_${TAGSHORT}"
    local STAT="${TAG}.stat_pr${RT}"
    local WAF="${TAG}_pr${RT}_${TS}.waf.log"
    local LOG="run_${TAG}_pr${RT}.log"
    local GSDEC="${TAG}_pr${RT}.gsdec.log"
    echo "[start ${TAGSHORT} scale=${SCALE}] gsdec=$GSDEC"
    GUD_RESORT_CORRECT=1 GS_DECISION_LOG="$GSDEC" ./cache_sim "$TRACE" "$CACHE" \
        --rw_policy write-only --trace_format csv \
        --cache_policy "$POLICY" \
        --cold_capacity "$COLD" \
        --waf_log_file "$WAF" \
        --periodic_ratio "$R" \
        --moving_avg_type "$MA" --moving_avg_window "$MAW" \
        --gs_decision_period_segs "$D" \
        --stat_log_file "$STAT" --scale "$SCALE" \
        > "$LOG" 2>&1 &
    echo "  pid=$!"
    sleep 1
}

run_one dwpd01to1 /home/sejun000/alibaba_dwpd01to1.trace    1
run_one dwpd1to2  /home/sejun000/alibaba_dwpd1to2_4x.trace  2
echo "2 RESORT runs launched"
