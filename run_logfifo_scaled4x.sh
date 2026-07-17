#!/bin/bash
# LOG_FIFO baseline on ssdtrace_scaled_4x.trace.
set -u
POLICY=LOG_FIFO
TRACE=/home/sejun000/ssdtrace_scaled_4x.trace
DEV=15000000000000
COLD=16050000000000
ALIGN=13079937024
CACHE=$(( ((DEV / 8 + ALIGN - 1) / ALIGN) * ALIGN ))
SCALE=2
TS=$(date +%y%m%d_%H%M%S)
TAG="LOG_FIFO_scaled4x"
STAT="${TAG}.stat_${TS}"
WAF="${TAG}_${TS}.waf.log"
LOG="run_${TAG}_${TS}.log"

cd "$(dirname "$0")"
echo "[start LOG_FIFO@scaled4x] stat=$STAT"
./cache_sim "$TRACE" "$CACHE" \
    --rw_policy write-only --trace_format csv \
    --cache_policy "$POLICY" \
    --cold_capacity "$COLD" \
    --waf_log_file "$WAF" \
    --stat_log_file "$STAT" --scale "$SCALE" \
    > "$LOG" 2>&1
echo "[done] $TAG"
