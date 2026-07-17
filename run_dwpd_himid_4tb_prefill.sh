#!/bin/bash
# dwpd hi/mid: 4TB device + synthetic cold injection + PREFILL of the reserve region.
#   device 4TB => cache 500GB(12.5%), cold 4.28TB, segment 1536MB(=768MB*2), scale=1.
#   reserve = 2.5 TB (absolute, --cold_reserve_bytes): synthetic round-robin region [0,2.5TB).
#   PREFILL: warm [0,2.5TB) via cache before main loop (NOT whole device -> leaves GC headroom).
#     trace remap starts at 2.5TB; trace region [2.5TB,4.28TB)=1.78TB >= WSS(hi~1.05/mid~1.2TB).
#     live at steady state = reserve 2.5 + WSS ~1.2 = ~3.7TB / 4.28TB ~= 86% (has GC headroom).
#   inject: every 9 trace writes -> 1 synthetic write = 10% of those 9, round-robin in [0,2.5TB).
#   loop_trace (unique caps after first pass); stop when cold cumulative writes reach 20 TiB.
# REFLASH = GS_FINAL D=1, r=8.64, MAW=1seg(393216).  vs  LOG_FIFO baseline.
# NOTE: prefill (sequential, all-unique) writes dilute the cumulative backend WAF toward 1.0;
#       measure steady-state WAF as the DELTA over the post-prefill window (waf.log col4/col3).
set -u
cd "$(dirname "$0")"

CACHE=500000000000              # 12.5% of 4 TB
COLD=4280000000000              # 4 TB * 1.07
SEG=1610612736                  # 1536 MB = 768MB * 2
MAW=393216                      # 1 segment in 4K blocks (= SEG/4096)
RESERVE=2500000000000           # 2.5 TB synthetic reserve (prefilled + round-robin)
COLD_LIMIT=21990232555520       # 20 TiB backend(cold) cumulative write stop (incl. prefill ~2TB)
CACHE_LIMIT=1099511627776000    # 1000 TB frontend safety cap
R=8.64
INJECT="--seq_inject_period 9 --seq_inject_frac 0.10 --cold_reserve_bytes $RESERVE"

COMMON="--rw_policy write-only --trace_format csv4col --cold_capacity $COLD \
        --segment_size $SEG --remap_lba --no_fill --loop_trace --prefill_cold_device \
        --cache_write_size_limit $CACHE_LIMIT --cold_write_size_limit $COLD_LIMIT $INJECT"

for T in hi mid; do
  TRACE=/home/sejun000/dwpd_${T}_first14TB_writes.csv

  TAG="dwpd${T}_4tb_pf_r864"
  echo "[launch] $TAG (REFLASH = GS_FINAL D=1, r=$R)"
  nohup stdbuf -oL ./cache_sim "$TRACE" "$CACHE" $COMMON \
      --cache_policy LOG_GREEDY_COST_BENEFIT_10_GS_FINAL --gs_decision_period_segs 1 \
      --periodic_ratio "$R" --util_step 0.02 --moving_avg_type ewma --moving_avg_window "$MAW" \
      --waf_log_file "${TAG}.waf.log" --stat_log_file "${TAG}.stat" \
      > "${TAG}.run.log" 2>&1 &
  echo "  pid=$!  log=${TAG}.run.log"

  TAG="dwpd${T}_4tb_pf_logfifo"
  echo "[launch] $TAG (LOG_FIFO)"
  nohup stdbuf -oL ./cache_sim "$TRACE" "$CACHE" $COMMON \
      --cache_policy LOG_FIFO \
      --waf_log_file "${TAG}.waf.log" --stat_log_file "${TAG}.stat" \
      > "${TAG}.run.log" 2>&1 &
  echo "  pid=$!  log=${TAG}.run.log"
done
echo "launched 4 runs (4TB + prefill 2.5TB reserve + inject, cold->20TiB; hi/mid x REFLASH/LOG_FIFO)"
