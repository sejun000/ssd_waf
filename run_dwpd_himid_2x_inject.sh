#!/bin/bash
# dwpd hi/mid: 2x config (device 2TB) + synthetic cold injection (NO prefill).
#   cache=250GB(12.5%), cold=2.14TB, segment=768MB(6GB/8), scale=1, remap_lba, no_fill.
#   inject: every 9 trace writes -> 1 synthetic write sized 10% of those 9 writes,
#           round-robin in reserved [0, 10%*cold); trace remap starts above the reserve.
#   loop_trace: at EOF replay trace from start (remap mappings persist).
#   stop: cold(backend) cumulative writes reach ~10 TiB.
# REFLASH = GS_FINAL D=1, r=8.64, MAW=1seg(196608).  vs  LOG_FIFO baseline.
set -u
cd "$(dirname "$0")"

CACHE=250000000000
COLD=2140000000000
SEG=805306368
MAW=196608
COLD_LIMIT=10995116277760       # ~10 TiB  -> backend(cold) write stop
CACHE_LIMIT=1099511627776000    # 1000 TB  -> frontend safety cap (cold limit governs)
R=8.64
INJECT="--seq_inject_period 9 --seq_inject_frac 0.10 --cold_reserve_frac 0.10"

COMMON="--rw_policy write-only --trace_format csv4col --cold_capacity $COLD \
        --segment_size $SEG --remap_lba --no_fill --loop_trace \
        --cache_write_size_limit $CACHE_LIMIT --cold_write_size_limit $COLD_LIMIT $INJECT"

for T in hi mid; do
  TRACE=/home/sejun000/dwpd_${T}_first14TB_writes.csv

  TAG="dwpd${T}_2x_inj_r864"
  echo "[launch] $TAG (REFLASH)"
  nohup stdbuf -oL ./cache_sim "$TRACE" "$CACHE" $COMMON \
      --cache_policy LOG_GREEDY_COST_BENEFIT_10_GS_FINAL --gs_decision_period_segs 1 \
      --periodic_ratio "$R" --util_step 0.02 --moving_avg_type ewma --moving_avg_window "$MAW" \
      --waf_log_file "${TAG}.waf.log" --stat_log_file "${TAG}.stat" \
      > "${TAG}.run.log" 2>&1 &
  echo "  pid=$!  log=${TAG}.run.log"

  TAG="dwpd${T}_2x_inj_logfifo"
  echo "[launch] $TAG (LOG_FIFO)"
  nohup stdbuf -oL ./cache_sim "$TRACE" "$CACHE" $COMMON \
      --cache_policy LOG_FIFO \
      --waf_log_file "${TAG}.waf.log" --stat_log_file "${TAG}.stat" \
      > "${TAG}.run.log" 2>&1 &
  echo "  pid=$!  log=${TAG}.run.log"
done
echo "launched 4 runs (2TB + inject, no prefill, cold->~10TiB; hi/mid x REFLASH/LOG_FIFO)"
