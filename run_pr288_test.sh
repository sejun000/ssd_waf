#!/bin/bash
# r=2.88 verification: baseline + RESORT on 3 traces (6 sims).
set -u
POLICY=LOG_GREEDY_COST_BENEFIT_10_GS_FINAL
MA=ewma
MAW=1572864
DEV=15000000000000
COLD=16050000000000
ALIGN=13079937024
CACHE=$(( ((DEV / 8 + ALIGN - 1) / ALIGN) * ALIGN ))
D=1
R=2.88
RT=288

cd "$(dirname "$0")"

run_one() {
    local MODE=$1       # base | resort
    local TAGSHORT=$2
    local TRACE=$3
    local SCALE=$4
    local TS=$(date +%y%m%d_%H%M%S)
    local TAG="GS_FINAL_${MODE}_${TAGSHORT}"
    local STAT="${TAG}.stat_pr${RT}"
    local WAF="${TAG}_pr${RT}_${TS}.waf.log"
    local LOG="run_${TAG}_pr${RT}.log"
    local GSDEC="${TAG}_pr${RT}.gsdec.log"
    local ENV_SET=""
    if [ "$MODE" = "resort" ]; then ENV_SET="GUD_RESORT_CORRECT=1"; fi
    echo "[start ${MODE} ${TAGSHORT} scale=${SCALE}] gsdec=$GSDEC"
    env $ENV_SET GS_DECISION_LOG="$GSDEC" ./cache_sim "$TRACE" "$CACHE" \
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

run_one base   dwpd01to1 /home/sejun000/alibaba_dwpd01to1.trace    1
run_one resort dwpd01to1 /home/sejun000/alibaba_dwpd01to1.trace    1
run_one base   dwpd1to2  /home/sejun000/alibaba_dwpd1to2_4x.trace  2
run_one resort dwpd1to2  /home/sejun000/alibaba_dwpd1to2_4x.trace  2
run_one base   dwpd2_5x  /home/sejun000/alibaba_dwpd2_5x.trace     2
run_one resort dwpd2_5x  /home/sejun000/alibaba_dwpd2_5x.trace     2
echo "6 r=2.88 runs launched"
