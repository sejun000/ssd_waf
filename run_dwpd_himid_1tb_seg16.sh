#!/bin/bash
# dwpd hi/mid (first14TB) at SCALED config:
#   device=1TB -> cache=12.5%(125GB), cold=1.07TB, segment=384MB(=6GB/16)
#   scale=1, --remap_lba (=> prefill disabled via --no_fill),
#   loop the trace until backend(cold-tier) writes reach ~5 TiB.
# Policies: REFLASH (= LOG_GREEDY_COST_BENEFIT_10_GS_FINAL, D=1, r=8.64, MAW=1seg=98304)
#           and LOG_FIFO (baseline).
set -u
cd "$(dirname "$0")"

CACHE=125000000000            # 12.5% of 1 TB (decimal)
COLD=1070000000000            # 1 TB * 1.07
SEG=402653184                 # 384 MB = 6 GB / 16
MAW=98304                     # 1 segment in 4K blocks (= 1572864 / 16)
COLD_LIMIT=5497558138880      # 5 TiB  -> backend(cold) write stop condition
CACHE_LIMIT=1099511627776000  # 1000 TB -> frontend safety cap (cold limit governs)
R=8.64

COMMON="--rw_policy write-only --trace_format csv4col --cold_capacity $COLD \
        --segment_size $SEG --remap_lba --no_fill --loop_trace \
        --cache_write_size_limit $CACHE_LIMIT --cold_write_size_limit $COLD_LIMIT"

for T in hi mid; do
  TRACE=/home/sejun000/dwpd_${T}_first14TB_writes.csv

  TAG="dwpd${T}_1tb_seg16_r864"
  echo "[launch] $TAG (REFLASH = GS_FINAL D=1, r=$R)"
  nohup ./cache_sim "$TRACE" "$CACHE" $COMMON \
      --cache_policy LOG_GREEDY_COST_BENEFIT_10_GS_FINAL \
      --gs_decision_period_segs 1 --periodic_ratio "$R" --util_step 0.02 \
      --moving_avg_type ewma --moving_avg_window "$MAW" \
      --waf_log_file "${TAG}.waf.log" --stat_log_file "${TAG}.stat" \
      > "${TAG}.run.log" 2>&1 &
  echo "  pid=$!  log=${TAG}.run.log"

  TAG="dwpd${T}_1tb_seg16_logfifo"
  echo "[launch] $TAG (LOG_FIFO)"
  nohup ./cache_sim "$TRACE" "$CACHE" $COMMON \
      --cache_policy LOG_FIFO \
      --waf_log_file "${TAG}.waf.log" --stat_log_file "${TAG}.stat" \
      > "${TAG}.run.log" 2>&1 &
  echo "  pid=$!  log=${TAG}.run.log"
done
echo "launched 4 runs (hi/mid x REFLASH/LOG_FIFO)"
