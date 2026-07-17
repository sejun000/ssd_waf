#!/bin/bash
# dwpd_hi: fill 14TiB(trace) -> then trace + 10% bg seq inject, run until COLD writes hit limit.
#   Same as run_dwpd_hi_fill14_then_inj10.sh but stop is COLD-write based (inj10 style),
#   so the cold device actually fills past capacity (5.12TB) and its FTL GC kicks in
#   -> cold WAF > 1 becomes observable (28TiB-host cap stopped at cold=4.6TB < capacity).
set -u
cd "$(dirname "$0")"

TRACE=/home/sejun000/dwpd_hi_first14TB_writes.csv
CACHE=640000000000                 # 640 GB
COLD=5120000000000                 # 5.12 TB
SEG=2147483648                     # 2 GiB
MAW=524288
RESERVE=3600000000000              # 3.6 TB synthetic reserve
WARMUP=15393162788864              # 14 TiB host -> fill-then-inject gate
CACHE_LIMIT=10995116277760000      # 10000 TiB host safety (non-binding; cold limit stops first)
COLD_LIMIT=21990232555520          # 20 TiB cold writes (~4x cold capacity -> steady cold GC)
R=8.64
POLICY=LOG_GREEDY_COST_BENEFIT_10_GS_FINAL   # REFLASH

TAG="dwpd_hi_fill14_coldlim20_r864"
INJECT="--seq_inject_period 9 --seq_inject_frac 0.10 --cold_reserve_bytes $RESERVE --seq_inject_warmup_bytes $WARMUP"

echo "[launch] $TAG  (fill 14TiB, inject 10%, stop@ cold 20TiB)"
GS_DECISION_LOG="${TAG}.gsdec.log" \
stdbuf -oL ./cache_sim "$TRACE" "$CACHE" \
    --rw_policy write-only --trace_format csv4col --cold_capacity "$COLD" \
    --segment_size "$SEG" --remap_lba --no_fill --loop_trace \
    --cache_write_size_limit "$CACHE_LIMIT" --cold_write_size_limit "$COLD_LIMIT" \
    $INJECT \
    --cache_policy "$POLICY" --gs_decision_period_segs 1 \
    --periodic_ratio "$R" --util_step 0.02 \
    --moving_avg_type ewma --moving_avg_window "$MAW" \
    --waf_log_file "${TAG}.waf.log" --stat_log_file "${TAG}.stat" \
    > "${TAG}.run.log" 2>&1
echo "[done] $TAG  exit=$?"
