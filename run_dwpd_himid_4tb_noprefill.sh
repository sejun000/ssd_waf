#!/bin/bash
# dwpd hi/mid: 4TB device + synthetic cold injection, NO prefill (prefill breaks REFLASH:
#   prefill empties the compactor heap -> Gud=0, invrate_sum=0 -> REFLASH degenerates to FIFO).
#   device 4TB => cache 500GB(12.5%), cold 4.28TB, segment 1536MB, reserve 2.5TB.
#   inject: every 9 trace writes -> 1 synthetic write = 10% of those 9, round-robin in [0,2.5TB).
#   loop_trace; stop when cold(backend) cumulative writes reach 10 TiB (matches 2x_inj baseline).
# REFLASH = GS_FINAL D=1, r=8.64, MAW=1seg(393216).  vs  LOG_FIFO baseline.
set -u
cd "$(dirname "$0")"

CACHE=500000000000
COLD=4280000000000
SEG=1610612736
MAW=393216
RESERVE=2500000000000
COLD_LIMIT=10995116277760        # 10 TiB backend(cold) cumulative write stop
CACHE_LIMIT=1099511627776000
R=8.64
INJECT="--seq_inject_period 9 --seq_inject_frac 0.10 --cold_reserve_bytes $RESERVE"

COMMON="--rw_policy write-only --trace_format csv4col --cold_capacity $COLD \
        --segment_size $SEG --remap_lba --no_fill --loop_trace \
        --cache_write_size_limit $CACHE_LIMIT --cold_write_size_limit $COLD_LIMIT $INJECT"

for T in hi mid; do
  TRACE=/home/sejun000/dwpd_${T}_first14TB_writes.csv

  TAG="dwpd${T}_4tb_np_r864"
  echo "[launch] $TAG (REFLASH = GS_FINAL D=1, r=$R)"
  nohup stdbuf -oL ./cache_sim "$TRACE" "$CACHE" $COMMON \
      --cache_policy LOG_GREEDY_COST_BENEFIT_10_GS_FINAL --gs_decision_period_segs 1 \
      --periodic_ratio "$R" --util_step 0.02 --moving_avg_type ewma --moving_avg_window "$MAW" \
      --waf_log_file "${TAG}.waf.log" --stat_log_file "${TAG}.stat" \
      > "${TAG}.run.log" 2>&1 &
  echo "  pid=$!  log=${TAG}.run.log"

  TAG="dwpd${T}_4tb_np_logfifo"
  echo "[launch] $TAG (LOG_FIFO)"
  nohup stdbuf -oL ./cache_sim "$TRACE" "$CACHE" $COMMON \
      --cache_policy LOG_FIFO \
      --waf_log_file "${TAG}.waf.log" --stat_log_file "${TAG}.stat" \
      > "${TAG}.run.log" 2>&1 &
  echo "  pid=$!  log=${TAG}.run.log"
done
echo "launched 4 no-prefill runs (4TB + inject, reserve 2.5TB, cold->10TiB; hi/mid x REFLASH/LOG_FIFO)"
