#!/bin/bash
# dwpd_mid on the deep-GC tiering config, June-style loop+inject:
#   mid has no 100TB extract (raw source unavailable), so loop the 14TB file
#   (15.4TB/pass, unique 1.03TB) to 100TB host. Reserve 1.7TB -> cold valid
#   ~= 1.7 + 1.03 - 0.25(cache-resident) ~= 2.48/3.0 ~ 83% util (same as hi).
#   cache 384GB, cold 3TB, seg = NAND block = 1.125GiB. 5 policies.
set -u
cd "$(dirname "$0")"

TRACE=/home/sejun000/dwpd_mid_first14TB_writes.csv
BIN=./cache_sim_blk1125lru
CACHE=384000000000                 # 384 GB
COLD=3000000000000                 # 3 TB
SEG=1207959552                     # 1.125 GiB
MAW=294912
CACHE_LIMIT=100000000000000        # 100 TB host writes
COLD_LIMIT=10995116277760000       # non-binding
R=8.64

INJECT="--seq_inject_period 9 --seq_inject_frac 0.10 --cold_reserve_bytes 1700000000000"
COMMON="--rw_policy write-only --trace_format csv4col --cold_capacity $COLD \
        --segment_size $SEG --remap_lba --no_fill --loop_trace \
        --cache_write_size_limit $CACHE_LIMIT --cold_write_size_limit $COLD_LIMIT"

launch() {  # $1=tag suffix, rest=policy-specific args
    local TAG=dwpdmid_384_$1; shift
    echo "[launch] $TAG"
    stdbuf -oL $BIN "$TRACE" "$CACHE" $COMMON $INJECT "$@" \
        --waf_log_file "${TAG}.waf.log" --stat_log_file "${TAG}.stat" \
        > "${TAG}.run.log" 2>&1 &
    echo "  pid=$!"
    sleep 1
}

launch REFLASH   --cache_policy LOG_GREEDY_COST_BENEFIT_10_GS_FINAL \
                 --gs_decision_period_segs 1 --periodic_ratio "$R" --util_step 0.02 \
                 --moving_avg_type ewma --moving_avg_window "$MAW"
launch REFLASH80 --cache_policy LOG_GREEDY_COST_BENEFIT_80
launch LOGFIFO   --cache_policy LOG_FIFO
launch LRU       --cache_policy LRU
launch LRU32W    --cache_policy LRU_32WAY

echo "all launched."
