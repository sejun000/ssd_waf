#!/bin/bash
# Clean GS_FINAL rule on ssdtrace_scaled_4x (SCALE=2, r=8.64) — matches the
# resortN100 baseline config but with the corrections removed (raw Gud,
# no victim subtraction). Tag: GS_FINAL_clean_scaled4x.
set -u
cd "$(dirname "$0")"
POLICY=LOG_GREEDY_COST_BENEFIT_10_GS_FINAL
MAW=1572864; CACHE=1883510931456; COLD=16050000000000
R=8.64; RT=864; D=1; SCALE=2
TRACE=/home/sejun000/ssdtrace_scaled_4x.trace
TS=$(date +%y%m%d_%H%M%S)
TAG=GS_FINAL_clean_scaled4x
GSDEC="${TAG}_pr${RT}.gsdec.log"
echo "[start scaled4x] gsdec=$GSDEC trace=$TRACE"
GS_DECISION_LOG="$GSDEC" ./cache_sim "$TRACE" "$CACHE" \
    --rw_policy write-only --trace_format csv \
    --cache_policy "$POLICY" --cold_capacity "$COLD" \
    --waf_log_file "${TAG}_pr${RT}_${TS}.waf.log" \
    --periodic_ratio "$R" --util_step 0.02 \
    --moving_avg_type ewma --moving_avg_window "$MAW" \
    --gs_decision_period_segs "$D" \
    --stat_log_file "${TAG}.stat_pr${RT}" --scale "$SCALE" \
    > "run_${TAG}_pr${RT}.log" 2>&1
echo "[done scaled4x] $(date)"
