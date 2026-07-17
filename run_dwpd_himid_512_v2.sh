#!/bin/bash
# REFLASH r=2.88 & r=8.64, NEW eviction code (check_and_evict while: processed<max_victims,
#   free_goal 조건 제거 — "(A)"). SAME 512 config (cache640/cold5.12/seg2GiB/reserve3.6/inject10).
#   gsdec on. nohup → 세션 종료 무관. tags _v2 (구버전 dwpd*_512_inj10_r* 데이터 보존).
set -u
cd "$(dirname "$0")"

CACHE=640000000000
COLD=5120000000000
SEG=2147483648
MAW=524288
RESERVE=3600000000000
COLD_LIMIT=54975581388800
CACHE_LIMIT=10995116277760000
INJECT="--seq_inject_period 9 --seq_inject_frac 0.10 --cold_reserve_bytes $RESERVE"
COMMON="--rw_policy write-only --trace_format csv4col --cold_capacity $COLD \
        --segment_size $SEG --remap_lba --no_fill --loop_trace \
        --cache_write_size_limit $CACHE_LIMIT --cold_write_size_limit $COLD_LIMIT $INJECT"

for T in hi mid; do
  TRACE=/home/sejun000/dwpd_${T}_first14TB_writes.csv
  for R in 2.88 8.64; do
    RTAG=$(echo "$R" | tr -d '.')         # 2.88->288, 8.64->864
    TAG="dwpd${T}_512_inj10_r${RTAG}_v2"
    echo "[launch] $TAG (REFLASH r=$R, new evict code, gsdec on)"
    GS_DECISION_LOG="${TAG}.gsdec.log" nohup stdbuf -oL ./cache_sim "$TRACE" "$CACHE" $COMMON \
        --cache_policy LOG_GREEDY_COST_BENEFIT_10_GS_FINAL --gs_decision_period_segs 1 \
        --periodic_ratio "$R" --util_step 0.02 --moving_avg_type ewma --moving_avg_window "$MAW" \
        --waf_log_file "${TAG}.waf.log" --stat_log_file "${TAG}.stat" \
        > "${TAG}.run.log" 2>&1 &
    echo "  pid=$!  log=${TAG}.run.log"
  done
done
echo "launched 4 REFLASH v2 runs (hi/mid x r2.88/r8.64, new evict code)"
