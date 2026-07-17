#!/bin/bash
# dwpd hi/mid (first14TB): previous 1tb/seg384 config DOUBLED on BOTH axes:
#   device 1 -> 2 TB   => cache 12.5% = 250GB, cold = 2.14TB
#   segment 384MB -> 768MB (= 6GB/8)  [so segment COUNT stays ~310]
#   scale=1, --remap_lba (=> --no_fill, no prefill), loop until cold(backend) ~5 TiB.
# Goal: raise cache/WSS (~15% -> ~30%) so REFLASH vs LOG_FIFO can diverge.
set -u
cd "$(dirname "$0")"

CACHE=250000000000            # 12.5% of 2 TB
COLD=2140000000000            # 2 TB * 1.07
SEG=805306368                 # 768 MB = 384MB * 2 = 6GB/8
MAW=196608                    # 1 segment in 4K blocks (= SEG/4096)
COLD_LIMIT=5497558138880      # 5 TiB backend(cold) write stop
CACHE_LIMIT=1099511627776000  # 1000 TB frontend safety cap
R=8.64

COMMON="--rw_policy write-only --trace_format csv4col --cold_capacity $COLD \
        --segment_size $SEG --remap_lba --no_fill --loop_trace \
        --cache_write_size_limit $CACHE_LIMIT --cold_write_size_limit $COLD_LIMIT"

for T in hi mid; do
  TRACE=/home/sejun000/dwpd_${T}_first14TB_writes.csv

  TAG="dwpd${T}_2tb_seg768_r864"
  echo "[launch] $TAG (REFLASH = GS_FINAL D=1, r=$R)"
  nohup ./cache_sim "$TRACE" "$CACHE" $COMMON \
      --cache_policy LOG_GREEDY_COST_BENEFIT_10_GS_FINAL \
      --gs_decision_period_segs 1 --periodic_ratio "$R" --util_step 0.02 \
      --moving_avg_type ewma --moving_avg_window "$MAW" \
      --waf_log_file "${TAG}.waf.log" --stat_log_file "${TAG}.stat" \
      > "${TAG}.run.log" 2>&1 &
  echo "  pid=$!  log=${TAG}.run.log"

  TAG="dwpd${T}_2tb_seg768_logfifo"
  echo "[launch] $TAG (LOG_FIFO)"
  nohup ./cache_sim "$TRACE" "$CACHE" $COMMON \
      --cache_policy LOG_FIFO \
      --waf_log_file "${TAG}.waf.log" --stat_log_file "${TAG}.stat" \
      > "${TAG}.run.log" 2>&1 &
  echo "  pid=$!  log=${TAG}.run.log"
done
echo "launched 4 runs (device=2TB cache=250GB segment=768MB; hi/mid x REFLASH/LOG_FIFO)"
