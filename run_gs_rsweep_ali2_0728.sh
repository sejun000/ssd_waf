#!/bin/bash
# GS_FINAL (REFLASH) periodic_ratio sweep on alibaba_dwpd1to2_4x only.
#   r in {1 2 4 6 8.64 12 16 32 64 100}, all runs in parallel (~95GB RSS each).
# Config otherwise identical to run_gs_clean_3traces.sh (D=1, MAW=1seg, SCALE=2).
set -u
POLICY=LOG_GREEDY_COST_BENEFIT_10_GS_FINAL
MA=ewma
MAW=1572864
DEV=15000000000000
COLD=16050000000000
ALIGN=13079937024
CACHE=$(( ((DEV / 8 + ALIGN - 1) / ALIGN) * ALIGN ))
SCALE=2
D=1
TRACE=/home/sejun000/alibaba_dwpd1to2_4x.trace

cd "$(dirname "$0")"

run_one() {
    local R=$1
    local RTAG=${R//./}
    local TS=$(date +%y%m%d_%H%M%S)
    local TAG="GS_FINAL_rswp0728_dwpd1to2_r${RTAG}"
    local STAT="${TAG}.stat"
    local WAF="${TAG}_${TS}.waf.log"
    local LOG="run_${TAG}.log"
    local GSDEC="${TAG}.gsdec.log"
    echo "[start r=${R}]"
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

for R in 1 2 4 6 8.64 12 16 32 64 100; do
    run_one "$R"
done

echo "all launched. waiting..."
wait
echo "all done at $(date)"
