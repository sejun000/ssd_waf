#!/bin/bash
# Experiment A: oracle death-time placement in the cold tier.
#   Same mid config as dwpdmid_384_* (cache 384GB, cold 3TB, seg = NAND block
#   1.125GiB, 14TB trace looped + 10% seq inject, reserve 1.7TB, r=8.64,
#   stop at 100TB host). Only change: --cold_oracle_streams K steers each cold
#   write to one of K backend streams by its remaining lifetime (host pages
#   until the next write to that block = exactly when its cold copy is trimmed).
#   Baselines with the flag off are dwpdmid_384_{LOGFIFO,REFLASH}.
set -u
cd "$(dirname "$0")"

TRACE=/home/sejun000/dwpd_mid_first14TB_writes.csv
BIN=./cache_sim
CACHE=384000000000
COLD=3000000000000
SEG=1207959552
MAW=294912
R=8.64

COMMON="--rw_policy write-only --trace_format csv4col --cold_capacity $COLD \
        --segment_size $SEG --remap_lba --no_fill --loop_trace \
        --cache_write_size_limit 100000000000000 --cold_write_size_limit 10995116277760000 \
        --seq_inject_period 9 --seq_inject_frac 0.10 --cold_reserve_bytes 1700000000000"

launch() {  # $1=tag  $2=K  rest=policy args
    local TAG=$1 K=$2; shift 2
    echo "[launch] $TAG (K=$K)"
    stdbuf -oL $BIN "$TRACE" "$CACHE" $COMMON --cold_oracle_streams "$K" "$@" \
        --waf_log_file "${TAG}.waf.log" --stat_log_file "${TAG}.stat" \
        > "${TAG}.run.log" 2>&1 &
    echo "  pid=$!"
    sleep 2
}

launch dwpdmid_384_LOGFIFO_orc8 8 --cache_policy LOG_FIFO
launch dwpdmid_384_LOGFIFO_orc4 4 --cache_policy LOG_FIFO
launch dwpdmid_384_REFLASH_orc8 8 --cache_policy LOG_GREEDY_COST_BENEFIT_10_GS_FINAL \
       --gs_decision_period_segs 1 --periodic_ratio "$R" --util_step 0.02 \
       --moving_avg_type ewma --moving_avg_window "$MAW"
launch dwpdmid_384_REFLASH_orc4 4 --cache_policy LOG_GREEDY_COST_BENEFIT_10_GS_FINAL \
       --gs_decision_period_segs 1 --periodic_ratio "$R" --util_step 0.02 \
       --moving_avg_type ewma --moving_avg_window "$MAW"

echo "all launched."
