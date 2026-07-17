#!/bin/bash
# NEW geometry: cache 1 TB / cold 8 TB / seg 3 GiB (8:1 ratio, 1.56x scale of 640/5.12).
#   reserve 6.2 TB (재현 ~90% cold util: reserve+WSS≈7.0~7.2/8 → WAF onset 보존).
#   inject 9/0.10, cold stop 50 TiB, no prefill, rule rhs*=waf_w. MAW=1seg(786432).
#   8 runs = {REFLASH r8.64, REFLASH r2.88, LOG_FIFO, LRU} x {hi, mid}.
#   REFLASH = GS_FINAL D=1 (+gsdec). evict code = option A (processed<max_victims), built 13:22.
#   tags dwpd{hi,mid}_1t8t_*  (구 dwpd*_512_inj10_* 데이터 보존).
set -u
cd "$(dirname "$0")"

CACHE=1000000000000               # 1 TB cache (fast tier)
COLD=8000000000000                # 8 TB cold (QLC)
SEG=3221225472                    # 3 GiB = 3*2^30 (786432 blocks, /4 /8 정수)
MAW=786432                        # seg in 4K blocks (1seg EWMA window)
RESERVE=6200000000000             # 6.2 TB synthetic cold reserve (~90% util 재현)
COLD_LIMIT=54975581388800         # 50 TiB cold stop (OK midway)
CACHE_LIMIT=10995116277760000     # 10000 TB frontend safety cap
INJECT="--seq_inject_period 9 --seq_inject_frac 0.10 --cold_reserve_bytes $RESERVE"

COMMON="--rw_policy write-only --trace_format csv4col --cold_capacity $COLD \
        --segment_size $SEG --remap_lba --no_fill --loop_trace \
        --cache_write_size_limit $CACHE_LIMIT --cold_write_size_limit $COLD_LIMIT $INJECT"

launch_reflash() {  # $1=tag  $2=R
  local TAG="$1" R="$2"
  echo "[launch] $TAG (REFLASH r=$R, gsdec on)"
  GS_DECISION_LOG="${TAG}.gsdec.log" nohup stdbuf -oL ./cache_sim "$TRACE" "$CACHE" $COMMON \
      --cache_policy LOG_GREEDY_COST_BENEFIT_10_GS_FINAL --gs_decision_period_segs 1 \
      --periodic_ratio "$R" --util_step 0.02 --moving_avg_type ewma --moving_avg_window "$MAW" \
      --waf_log_file "${TAG}.waf.log" --stat_log_file "${TAG}.stat" \
      > "${TAG}.run.log" 2>&1 &
  echo "  pid=$!"
}
launch_simple() {   # $1=tag  $2=policy
  local TAG="$1" POL="$2"
  echo "[launch] $TAG ($POL)"
  nohup stdbuf -oL ./cache_sim "$TRACE" "$CACHE" $COMMON \
      --cache_policy "$POL" \
      --waf_log_file "${TAG}.waf.log" --stat_log_file "${TAG}.stat" \
      > "${TAG}.run.log" 2>&1 &
  echo "  pid=$!"
}

for T in hi mid; do
  TRACE=/home/sejun000/dwpd_${T}_first14TB_writes.csv
  launch_reflash "dwpd${T}_1t8t_r864"    8.64
  launch_reflash "dwpd${T}_1t8t_r288"    2.88
  launch_simple  "dwpd${T}_1t8t_logfifo" LOG_FIFO
  launch_simple  "dwpd${T}_1t8t_lru"     LRU
done
echo "launched 8 runs (cache1T/cold8T/seg3GiB/reserve6.2T/inject10, cold->50TiB; hi/mid x REFLASH r864/r288/FIFO/LRU)"
