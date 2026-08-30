#!/bin/bash
# Real first-100TB trace + 10% background seq inject, big tiering config:
#   cache 640GB, cold 5.12TB, seg = NAND block = 2GiB (cache_sim_blk2g).
#   Inject cycles reserve [0, 3.2TB) (~35TB-host rewrite period, unabsorbable);
#   cold static util ~= (3.2 + 1.43 trace-unique - cache-resident)/5.12 ~ 85%
#   so backend GC is binding (3.6TB reserve would hit ~98% -> overflow risk).
#   No warmup gate: inject active from t=0. Stop at 100TB host (col1 incl inject).
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

INJECT="--seq_inject_period 9 --seq_inject_frac 0.10 --cold_reserve_bytes 3200000000000"
COMMON="--rw_policy write-only --trace_format csv4col --cold_capacity $COLD \
        --segment_size $SEG --remap_lba --no_fill \
        --cache_write_size_limit $CACHE_LIMIT --cold_write_size_limit $COLD_LIMIT"

TAG=dwpd_hi_100TBinj_REFLASH
echo "[launch] $TAG"
stdbuf -oL ./cache_sim_blk2g "$TRACE" "$CACHE" $COMMON $INJECT \
    --cache_policy LOG_GREEDY_COST_BENEFIT_10_GS_FINAL \
    --gs_decision_period_segs 1 \
    --periodic_ratio "$R" --util_step 0.02 \
    --moving_avg_type ewma --moving_avg_window "$MAW" \
    --waf_log_file "${TAG}.waf.log" --stat_log_file "${TAG}.stat" \
    > "${TAG}.run.log" 2>&1 &
echo "  pid=$!"
sleep 1

TAG=dwpd_hi_100TBinj_REFLASH80
echo "[launch] $TAG"
stdbuf -oL ./cache_sim_blk2g "$TRACE" "$CACHE" $COMMON $INJECT \
    --cache_policy LOG_GREEDY_COST_BENEFIT_80 \
    --waf_log_file "${TAG}.waf.log" --stat_log_file "${TAG}.stat" \
    > "${TAG}.run.log" 2>&1 &
echo "  pid=$!"
sleep 1

TAG=dwpd_hi_100TBinj_LOGFIFO
echo "[launch] $TAG"
stdbuf -oL ./cache_sim_blk2g "$TRACE" "$CACHE" $COMMON $INJECT \
    --cache_policy LOG_FIFO \
    --waf_log_file "${TAG}.waf.log" --stat_log_file "${TAG}.stat" \
    > "${TAG}.run.log" 2>&1 &
echo "  pid=$!"

echo "all launched."
