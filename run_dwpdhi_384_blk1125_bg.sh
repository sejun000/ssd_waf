#!/bin/bash
# Replicate the June dwpdhi_384 deep-GC config (cache 384GB, cold 3TB,
# seg 1.125GiB, 14TB trace looped, inject 10% reserve 1.7TB, stop at 50TiB
# cold) with ONE change: backend NAND block = seg = 1.125GiB (June was 6GiB).
# Binary: cache_sim_blk1125m (-DNAND_BLOCK_SIZE=1207959552).
set -u
cd "$(dirname "$0")"

TRACE=/home/sejun000/dwpd_hi_first14TB_writes.csv
CACHE=384000000000                 # 384 GB
COLD=3000000000000                 # 3 TB
SEG=1207959552                     # 1.125 GiB
MAW=294912                         # 1 seg worth of 4KiB blocks
CACHE_LIMIT=10995116277760000      # non-binding
COLD_LIMIT=54975581388800          # 50 TiB cold writes (June limit)
R=8.64

INJECT="--seq_inject_period 9 --seq_inject_frac 0.10 --cold_reserve_bytes 1700000000000"
COMMON="--rw_policy write-only --trace_format csv4col --cold_capacity $COLD \
        --segment_size $SEG --remap_lba --no_fill --loop_trace \
        --cache_write_size_limit $CACHE_LIMIT --cold_write_size_limit $COLD_LIMIT"

TAG=dwpdhi_384blk1125_REFLASH
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

TAG=dwpdhi_384blk1125_REFLASH80
echo "[launch] $TAG"
stdbuf -oL ./cache_sim_blk1125m "$TRACE" "$CACHE" $COMMON $INJECT \
    --cache_policy LOG_GREEDY_COST_BENEFIT_80 \
    --waf_log_file "${TAG}.waf.log" --stat_log_file "${TAG}.stat" \
    > "${TAG}.run.log" 2>&1 &
echo "  pid=$!"
sleep 1

TAG=dwpdhi_384blk1125_LOGFIFO
echo "[launch] $TAG"
stdbuf -oL ./cache_sim_blk1125m "$TRACE" "$CACHE" $COMMON $INJECT \
    --cache_policy LOG_FIFO \
    --waf_log_file "${TAG}.waf.log" --stat_log_file "${TAG}.stat" \
    > "${TAG}.run.log" 2>&1 &
echo "  pid=$!"

echo "all launched."
