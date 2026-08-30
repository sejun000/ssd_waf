#!/bin/bash
# Real first-100TB trace + 10% background seq inject on the deep-GC tiering:
#   cache 384GB, cold 3TB, seg = NAND block = 1.125GiB (cache_sim_blk1125m).
#   Single pass (no loop), stop at 100TB host (col1 incl inject).
#   Reserve 1.3TB (not June's 1.7TB): trace unique set is 1.43TB, so
#   cold valid ~= 1.3 + 1.43 - 0.25(cache-resident) ~= 2.48/3.0 ~ 83% util.
set -u
cd "$(dirname "$0")"

TRACE=/home/sejun000/dwpd_hi_first100TB.csv
CACHE=384000000000                 # 384 GB
COLD=3000000000000                 # 3 TB
SEG=1207959552                     # 1.125 GiB
MAW=294912                         # 1 seg worth of 4KiB blocks
CACHE_LIMIT=100000000000000        # 100 TB host writes
COLD_LIMIT=10995116277760000       # non-binding
R=8.64

INJECT="--seq_inject_period 9 --seq_inject_frac 0.10 --cold_reserve_bytes 1300000000000"
COMMON="--rw_policy write-only --trace_format csv4col --cold_capacity $COLD \
        --segment_size $SEG --remap_lba --no_fill \
        --cache_write_size_limit $CACHE_LIMIT --cold_write_size_limit $COLD_LIMIT"

TAG=dwpdhi384_100tb_REFLASH
echo "[launch] $TAG"
stdbuf -oL ./cache_sim_blk1125m "$TRACE" "$CACHE" $COMMON $INJECT \
    --cache_policy LOG_GREEDY_COST_BENEFIT_10_GS_FINAL \
    --gs_decision_period_segs 1 \
    --periodic_ratio "$R" --util_step 0.02 \
    --moving_avg_type ewma --moving_avg_window "$MAW" \
    --waf_log_file "${TAG}.waf.log" --stat_log_file "${TAG}.stat" \
    > "${TAG}.run.log" 2>&1 &
echo "  pid=$!"
sleep 1

TAG=dwpdhi384_100tb_REFLASH80
echo "[launch] $TAG"
stdbuf -oL ./cache_sim_blk1125m "$TRACE" "$CACHE" $COMMON $INJECT \
    --cache_policy LOG_GREEDY_COST_BENEFIT_80 \
    --waf_log_file "${TAG}.waf.log" --stat_log_file "${TAG}.stat" \
    > "${TAG}.run.log" 2>&1 &
echo "  pid=$!"
sleep 1

TAG=dwpdhi384_100tb_LOGFIFO
echo "[launch] $TAG"
stdbuf -oL ./cache_sim_blk1125m "$TRACE" "$CACHE" $COMMON $INJECT \
    --cache_policy LOG_FIFO \
    --waf_log_file "${TAG}.waf.log" --stat_log_file "${TAG}.stat" \
    > "${TAG}.run.log" 2>&1 &
echo "  pid=$!"

echo "all launched."
