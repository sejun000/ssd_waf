#!/bin/bash
# GS_FINAL (resortwtv-equivalent) on dwpd01to1 trace at scale=2.
# Re-run with scale=2 so it matches LF baseline (LOG_FIFO.stat.log.20260529_225648).
set -u
POLICY=LOG_GREEDY_COST_BENEFIT_10_GS_FINAL
MA=ewma
MAW=1572864
TRACE=/home/sejun000/alibaba_dwpd01to1.trace
DEV=15000000000000
COLD=16050000000000
ALIGN=13079937024
CACHE=$(( ((DEV / 8 + ALIGN - 1) / ALIGN) * ALIGN ))
SCALE=2
R=8.64
RT=864
D=1
TS=$(date +%y%m%d_%H%M%S)
TAG="GS_FINAL_resortwtv_dwpd01to1_scale2"
STAT="${TAG}.stat_pr${RT}"
WAF="${TAG}_pr${RT}_${TS}.waf.log"
LOG="run_${TAG}_pr${RT}.log"
GSDEC="${TAG}_pr${RT}.gsdec.log"

cd "$(dirname "$0")"
echo "[start GS_FINAL@dwpd01to1 scale=2] gsdec=$GSDEC stat=$STAT"
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
