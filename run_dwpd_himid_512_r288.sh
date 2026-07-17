#!/bin/bash
# REFLASH r=2.88 (= 8.64/3, less aggressive compaction) — SAME config as 512_inj10.
#   cache 640GB, cold 5.12TB, seg 2GiB, reserve 3.6TB, inject 10%, rule rhs*=waf_w, no prefill.
#   ONLY periodic_ratio differs (2.88 vs the r864 runs' 8.64).  GS_DECISION_LOG enabled.
#   vs baseline dwpd{hi,mid}_512_inj10_r864 (r=8.64).
set -u
cd "$(dirname "$0")"

CACHE=640000000000
COLD=5120000000000
SEG=2147483648
MAW=524288
RESERVE=3600000000000
COLD_LIMIT=54975581388800
CACHE_LIMIT=10995116277760000
R=2.88
INJECT="--seq_inject_period 9 --seq_inject_frac 0.10 --cold_reserve_bytes $RESERVE"

COMMON="--rw_policy write-only --trace_format csv4col --cold_capacity $COLD \
        --segment_size $SEG --remap_lba --no_fill --loop_trace \
        --cache_write_size_limit $CACHE_LIMIT --cold_write_size_limit $COLD_LIMIT $INJECT"

for T in hi mid; do
  TRACE=/home/sejun000/dwpd_${T}_first14TB_writes.csv
  TAG="dwpd${T}_512_inj10_r288"
  echo "[launch] $TAG (REFLASH r=2.88, gsdec on)"
  GS_DECISION_LOG="${TAG}.gsdec.log" nohup stdbuf -oL ./cache_sim "$TRACE" "$CACHE" $COMMON \
      --cache_policy LOG_GREEDY_COST_BENEFIT_10_GS_FINAL --gs_decision_period_segs 1 \
      --periodic_ratio "$R" --util_step 0.02 --moving_avg_type ewma --moving_avg_window "$MAW" \
      --waf_log_file "${TAG}.waf.log" --stat_log_file "${TAG}.stat" \
      > "${TAG}.run.log" 2>&1 &
  echo "  pid=$!  log=${TAG}.run.log  gsdec=${TAG}.gsdec.log"
done
echo "launched 2 REFLASH r=2.88 runs (hi/mid, gsdec on)"
