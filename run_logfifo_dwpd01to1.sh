#!/bin/bash
# LOG_FIFO baseline on dwpd01to1 (= missing comparison for amp analysis).
# Matches run_logfifo_dwpd2_5x.sh config but on dwpd01to1 trace.
set -u
POLICY=LOG_FIFO
DEV=15000000000000
COLD=16050000000000
ALIGN=13079937024
CACHE=$(( ((DEV / 8 + ALIGN - 1) / ALIGN) * ALIGN ))
TS=$(date +%y%m%d_%H%M%S)
TAG="LOG_FIFO_dwpd01to1"
STAT="${TAG}.stat_${TS}"
WAF="${TAG}_${TS}.waf.log"
LOG="run_${TAG}_${TS}.log"

cd "$(dirname "$0")"
echo "[start LOG_FIFO@dwpd01to1] stat=$STAT"
./cache_sim /home/sejun000/alibaba_dwpd01to1.trace "$CACHE" \
    --rw_policy write-only --trace_format csv \
    --cache_policy "$POLICY" \
    --cold_capacity "$COLD" \
    --waf_log_file "$WAF" \
    --stat_log_file "$STAT" --scale 2 \
    > "$LOG" 2>&1
echo "  done: stat=$STAT"
