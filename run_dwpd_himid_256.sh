#!/bin/bash
# 256 GB cache geometry = 1t8t의 0.25x 비율 축소 (cache 1T→256G, cold 8T→2T, seg 3GiB→0.75GiB).
#   reserve 800GB (cold footprint=reserve+WSS: hi 1.64 / mid 1.83 < 2TB, util 82~91.5%, onset 빠름).
#   cache↓ → eviction 빨라 onset 당겨짐. seg 0.75GiB=317 segs(이전 ~310 granularity 유지).
#   LOG_FIFO keep_stat_log fix 반영된 빌드 → 새 FIFO U_B/stat 깨끗.
#   8 runs = {REFLASH r8.64, REFLASH r2.88, LOG_FIFO, LRU} x {hi, mid}. REFLASH는 gsdec on.
#   tags dwpd{hi,mid}_256_*.
set -u
cd "$(dirname "$0")"

CACHE=256000000000                # 256 GB cache (fast tier)
COLD=2000000000000                # 2 TB cold (QLC)
SEG=805306368                     # 0.75 GiB = 3*2^28 (196608 blocks; /4 /8 정수)
MAW=196608                        # seg in 4K blocks (1seg EWMA window)
RESERVE=800000000000              # 800 GB synthetic cold reserve (footprint<2TB, ~90% util mid)
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
  launch_reflash "dwpd${T}_256_r864"    8.64
  launch_reflash "dwpd${T}_256_r288"    2.88
  launch_simple  "dwpd${T}_256_logfifo" LOG_FIFO
  launch_simple  "dwpd${T}_256_lru"     LRU
done
echo "launched 8 runs (cache256G/cold2T/seg0.75GiB/reserve800G/inject10, cold->50TiB; hi/mid x REFLASH r864/r288/FIFO/LRU)"
