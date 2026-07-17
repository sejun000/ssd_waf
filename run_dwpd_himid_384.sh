#!/bin/bash
# 384 GB cache geometry = 256 config의 1.5x (cache 256→384, cold 2T→3T, seg 0.75→1.125GiB).
#   동일 비율(cache:cold=0.128). reserve 1.7TB → footprint hi 2.54(85%)/mid 2.73(91%), onset 확실,
#   overflow 한계 3-1.03=1.97TB까지 0.27TB 여유.
#   seg 1.125GiB=317 segs(256과 동일 granularity). LOG_FIFO stat은 stat.log.<ts>로 나감(추출은 가능).
#   8 runs = {REFLASH r8.64, REFLASH r2.88, LOG_FIFO, LRU} x {hi, mid}. REFLASH gsdec on.
#   tags dwpd{hi,mid}_384_*.
set -u
cd "$(dirname "$0")"

CACHE=384000000000                # 384 GB cache
COLD=3000000000000                # 3 TB cold (QLC) — 384*7.8125
SEG=1207959552                    # 1.125 GiB = 9*2^27 (294912 blocks; /4 /8 정수)
MAW=294912                        # seg in 4K blocks
RESERVE=1700000000000             # 1.7 TB reserve (util hi 85% / mid 91%)
COLD_LIMIT=54975581388800         # 50 TiB cold stop
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
  launch_reflash "dwpd${T}_384_r864"    8.64
  launch_reflash "dwpd${T}_384_r288"    2.88
  launch_simple  "dwpd${T}_384_logfifo" LOG_FIFO
  launch_simple  "dwpd${T}_384_lru"     LRU
done
echo "launched 8 runs (cache384G/cold3T/seg1.125GiB/reserve1.7T/inject10, cold->50TiB; hi/mid x REFLASH r864/r288/FIFO/LRU)"
