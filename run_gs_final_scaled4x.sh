#!/bin/bash
# GS_FINAL (resortwtv-equivalent) on ssdtrace_scaled_4x.trace.
# Same config as run_GS_FINAL_resortwtv_dwpd1to2_pr864 (D=1, MAW=1seg, r=8.64).
set -u
POLICY=LOG_GREEDY_COST_BENEFIT_10_GS_FINAL
MA=ewma
MAW=1572864
TRACE=/home/sejun000/ssdtrace_scaled_4x.trace
DEV=15000000000000
COLD=16050000000000
ALIGN=13079937024
CACHE=$(( ((DEV / 8 + ALIGN - 1) / ALIGN) * ALIGN ))
SCALE=2
R=8.64
RT=864
D=1
TS=$(date +%y%m%d_%H%M%S)
TAG="GS_FINAL_resortwtv_scaled4x"
STAT="${TAG}.stat_pr${RT}"
WAF="${TAG}_pr${RT}_${TS}.waf.log"
LOG="run_${TAG}_pr${RT}.log"
GSDEC="${TAG}_pr${RT}.gsdec.log"

cd "$(dirname "$0")"
echo "[start GS_FINAL@scaled4x] gsdec=$GSDEC stat=$STAT"
GS_DECISION_LOG="$GSDEC" ./cache_sim "$TRACE" "$CACHE" \
    --rw_policy write-only --trace_format csv \
    --cache_policy "$POLICY" \
    --cold_capacity "$COLD" \
    --waf_log_file "$WAF" \
    --periodic_ratio "$R" --util_step 0.02 \
    --moving_avg_type "$MA" --moving_avg_window "$MAW" \
    --gs_decision_period_segs "$D" \
    --stat_log_file "$STAT" --scale "$SCALE" \
    > "$LOG" 2>&1
echo "[done] $TAG"
