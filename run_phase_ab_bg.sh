#!/bin/bash
# Scaled-down tiering vs unique set (1.435TB) of the real first-100TB trace:
#   cache 160GB (11% of unique set), cold 1.6TB (static util ~85%),
#   seg = NAND block = 512MiB (cache_sim_blk512m, -DNAND_BLOCK_SIZE=536870912).
#   Single pass, no seq-inject, stop at 100TB host. Backend GC now binding.
set -u
cd "$(dirname "$0")"

TRACE=/home/sejun000/phase_ab_20TB.csv
CACHE=160000000000                 # 160 GB
COLD=1600000000000                 # 1.6 TB
SEG=536870912                      # 512 MiB
MAW=131072                         # 1 seg worth of 4KiB blocks
CACHE_LIMIT=30000000000000        # 100 TB host writes
COLD_LIMIT=10995116277760000       # non-binding
R=8.64

COMMON="--rw_policy write-only --trace_format csv4col --cold_capacity $COLD \
        --segment_size $SEG --remap_lba --no_fill \
        --cache_write_size_limit $CACHE_LIMIT --cold_write_size_limit $COLD_LIMIT"

TAG=phase_ab_REFLASH
echo "[launch] $TAG"
stdbuf -oL ./cache_sim_blk512m "$TRACE" "$CACHE" $COMMON \
    --cache_policy LOG_GREEDY_COST_BENEFIT_10_GS_FINAL \
    --gs_decision_period_segs 1 \
    --periodic_ratio "$R" --util_step 0.02 \
    --moving_avg_type ewma --moving_avg_window "$MAW" \
    --waf_log_file "${TAG}.waf.log" --stat_log_file "${TAG}.stat" \
    > "${TAG}.run.log" 2>&1 &
echo "  pid=$!"
sleep 1

TAG=phase_ab_REFLASH80
echo "[launch] $TAG"
stdbuf -oL ./cache_sim_blk512m "$TRACE" "$CACHE" $COMMON \
    --cache_policy LOG_GREEDY_COST_BENEFIT_80 \
    --waf_log_file "${TAG}.waf.log" --stat_log_file "${TAG}.stat" \
    > "${TAG}.run.log" 2>&1 &
echo "  pid=$!"
sleep 1

TAG=phase_ab_LOGFIFO
echo "[launch] $TAG"
stdbuf -oL ./cache_sim_blk512m "$TRACE" "$CACHE" $COMMON \
    --cache_policy LOG_FIFO \
    --waf_log_file "${TAG}.waf.log" --stat_log_file "${TAG}.stat" \
    > "${TAG}.run.log" 2>&1 &
echo "  pid=$!"

echo "all launched."
