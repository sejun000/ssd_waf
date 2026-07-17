#!/bin/bash
# LOG_FIFO baseline on dwpd2_5x (= missing comparison for our analysis).
# Matches run_LOG_FIFO_match_gsfinal config but on dwpd2_5x trace.
set -u
POLICY=LOG_FIFO
DEV=15000000000000
COLD=16050000000000
ALIGN=13079937024
CACHE=$(( ((DEV / 8 + ALIGN - 1) / ALIGN) * ALIGN ))
TS=$(date +%y%m%d_%H%M%S)
TAG="LOG_FIFO_dwpd2_5x"
STAT="${TAG}.stat_${TS}"
WAF="${TAG}_${TS}.waf.log"
LOG="run_${TAG}_${TS}.log"

cd "$(dirname "$0")"
echo "[start LOG_FIFO@dwpd2_5x] stat=$STAT"
./cache_sim /home/sejun000/alibaba_dwpd2_5x.trace "$CACHE" \
    --rw_policy write-only --trace_format csv \
    --cache_policy "$POLICY" \
    --cold_capacity "$COLD" \
    --waf_log_file "$WAF" \
    --stat_log_file "$STAT" --scale 2 \
    > "$LOG" 2>&1 &
echo "  pid=$!"
