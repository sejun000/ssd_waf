#!/bin/bash
# Top-K invalidation-concentration correction test (GUD_TOPK_CORRECT=1).
# Mirror run_reflash_3traces.sh config; only env toggle changes.
# Baseline = GS_FINAL_vidlog_dwpd2_5x_pr864.gsdec.log (already on disk).
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
    local TAG="GS_FINAL_topkcorr_${TAGSHORT}"
    local STAT="${TAG}.stat_pr${RT}"
    local WAF="${TAG}_pr${RT}_${TS}.waf.log"
    local LOG="run_${TAG}_pr${RT}.log"
    local GSDEC="${TAG}_pr${RT}.gsdec.log"
    echo "[start ${TAGSHORT} scale=${SCALE}] gsdec=$GSDEC"
    GUD_TOPK_CORRECT=1 GS_DECISION_LOG="$GSDEC" ./cache_sim "$TRACE" "$CACHE" \
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
}

run_one dwpd2_5x  /home/sejun000/alibaba_dwpd2_5x.trace  2
echo "TOPK_CORRECT=1 dwpd2_5x launched"
