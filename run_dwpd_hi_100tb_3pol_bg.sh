#!/bin/bash
# Real first-100TB dwpd_hi trace, single pass (no loop, no seq-inject):
#   LOG_FIFO vs REFLASH(GS_FINAL D=1) vs REFLASH80(fixed valid_rate 0.80),
#   stop at 100TB host writes. Backend GC starts once cold (5.12TB) fills
#   organically from evictions. Compare waf.log col3/col4 at equal col1.
set -u
cd "$(dirname "$0")"

TRACE=/home/sejun000/dwpd_hi_first100TB.csv
CACHE=640000000000                 # 640 GB
COLD=5120000000000                 # 5.12 TB
SEG=2147483648                     # 2 GiB
MAW=524288
CACHE_LIMIT=100000000000000        # 100 TB host writes
COLD_LIMIT=10995116277760000       # non-binding
R=8.64

COMMON="--rw_policy write-only --trace_format csv4col --cold_capacity $COLD \
        --segment_size $SEG --remap_lba --no_fill \
        --cache_write_size_limit $CACHE_LIMIT --cold_write_size_limit $COLD_LIMIT"

TAG=dwpd_hi_100TB_REFLASH
echo "[launch] $TAG"
stdbuf -oL ./cache_sim_blk2g "$TRACE" "$CACHE" $COMMON \
    --cache_policy LOG_GREEDY_COST_BENEFIT_10_GS_FINAL \
    --gs_decision_period_segs 1 \
    --periodic_ratio "$R" --util_step 0.02 \
    --moving_avg_type ewma --moving_avg_window "$MAW" \
    --waf_log_file "${TAG}.waf.log" --stat_log_file "${TAG}.stat" \
    > "${TAG}.run.log" 2>&1 &
echo "  pid=$!"
sleep 1

TAG=dwpd_hi_100TB_REFLASH80
echo "[launch] $TAG"
stdbuf -oL ./cache_sim_blk2g "$TRACE" "$CACHE" $COMMON \
    --cache_policy LOG_GREEDY_COST_BENEFIT_80 \
    --waf_log_file "${TAG}.waf.log" --stat_log_file "${TAG}.stat" \
    > "${TAG}.run.log" 2>&1 &
echo "  pid=$!"
sleep 1

TAG=dwpd_hi_100TB_LOGFIFO
echo "[launch] $TAG"
stdbuf -oL ./cache_sim_blk2g "$TRACE" "$CACHE" $COMMON \
    --cache_policy LOG_FIFO \
    --waf_log_file "${TAG}.waf.log" --stat_log_file "${TAG}.stat" \
    > "${TAG}.run.log" 2>&1 &
echo "  pid=$!"

echo "all launched."
