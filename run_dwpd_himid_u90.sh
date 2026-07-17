#!/bin/bash
# dwpd hi/mid: higher-util config (~90%). rule = rhs*waf_w (qlc-amplified), no prefill.
#   cache 640GB, cold 5.12TB, segment 2GiB(=2^31, 298 segs), reserve 3.6TB, inject 20%.
#   live = reserve 3.6 + WSS(hi 0.84/mid 1.03) = 4.4~4.6TB / 5.12 = 87~90% util.
#   trace region = 5.12-3.6 = 1.52TB > WSS. loop_trace; stop at cold = 50 TiB (OK to stop midway).
# REFLASH = GS_FINAL D=1, r=8.64, MAW=1seg(524288).  vs  LOG_FIFO baseline.
set -u
cd "$(dirname "$0")"

CACHE=640000000000
COLD=5120000000000
SEG=2147483648                    # 2 GiB = 2^31
MAW=524288                        # seg in 4K blocks = 2^19
RESERVE=3600000000000             # 3.6 TB synthetic reserve
COLD_LIMIT=54975581388800         # 50 TiB cold stop
CACHE_LIMIT=10995116277760000     # 10000 TB frontend safety cap
R=8.64
INJECT="--seq_inject_period 9 --seq_inject_frac 0.20 --cold_reserve_bytes $RESERVE"

COMMON="--rw_policy write-only --trace_format csv4col --cold_capacity $COLD \
        --segment_size $SEG --remap_lba --no_fill --loop_trace \
        --cache_write_size_limit $CACHE_LIMIT --cold_write_size_limit $COLD_LIMIT $INJECT"

for T in hi mid; do
  TRACE=/home/sejun000/dwpd_${T}_first14TB_writes.csv

  TAG="dwpd${T}_u90_r864"
  echo "[launch] $TAG (REFLASH, rhs*=waf_w)"
  nohup stdbuf -oL ./cache_sim "$TRACE" "$CACHE" $COMMON \
      --cache_policy LOG_GREEDY_COST_BENEFIT_10_GS_FINAL --gs_decision_period_segs 1 \
      --periodic_ratio "$R" --util_step 0.02 --moving_avg_type ewma --moving_avg_window "$MAW" \
      --waf_log_file "${TAG}.waf.log" --stat_log_file "${TAG}.stat" \
      > "${TAG}.run.log" 2>&1 &
  echo "  pid=$!  log=${TAG}.run.log"

  TAG="dwpd${T}_u90_logfifo"
  echo "[launch] $TAG (LOG_FIFO)"
  nohup stdbuf -oL ./cache_sim "$TRACE" "$CACHE" $COMMON \
      --cache_policy LOG_FIFO \
      --waf_log_file "${TAG}.waf.log" --stat_log_file "${TAG}.stat" \
      > "${TAG}.run.log" 2>&1 &
  echo "  pid=$!  log=${TAG}.run.log"
done
echo "launched 4 runs (cache640/cold5.12/seg2GiB/reserve3.6/inject20, cold->50TiB; hi/mid x REFLASH/FIFO)"
