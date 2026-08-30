#!/bin/bash
# Backend pressure from a direct prefill instead of the 10% background inject.
#   [0, 2.0TB) is written straight into the cold FTL before t=0 (cache bypassed,
#   counters zeroed) and never rewritten, so it is static "other tenant" data.
#   Steady-state occupancy = 2.0 prefill + ~0.78 trace-resident = 2.78/3.0 ~ 93%.
#   Every host write is now a real trace write (no synthetic 10% flush floor),
#   which is what the inject setup diluted. Baselines with inject: dwpdmid_384_*.
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
        --cold_reserve_bytes 2000000000000 --prefill_cold_direct"

launch() {  # $1=tag, rest=policy args
    local TAG=$1; shift
    echo "[launch] $TAG"
    stdbuf -oL $BIN "$TRACE" "$CACHE" $COMMON "$@" \
        --waf_log_file "${TAG}.waf.log" --stat_log_file "${TAG}.stat" \
        > "${TAG}.run.log" 2>&1 &
    echo "  pid=$!"
    sleep 2
}

launch dwpdmid_pf93_LOGFIFO   --cache_policy LOG_FIFO
launch dwpdmid_pf93_REFLASH   --cache_policy LOG_GREEDY_COST_BENEFIT_10_GS_FINAL \
       --gs_decision_period_segs 1 --periodic_ratio "$R" --util_step 0.02 \
       --moving_avg_type ewma --moving_avg_window "$MAW"
launch dwpdmid_pf93_REFLASH80 --cache_policy LOG_GREEDY_COST_BENEFIT_80

echo "all launched."
