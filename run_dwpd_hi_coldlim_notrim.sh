#!/bin/bash
# Backend(capacity-tier) GC comparison, CSAL-like NO-TRIM mode: LOG_FIFO vs REFLASH.
#   Identical to run_dwpd_hi_coldlim_fifo_vs_reflash.sh plus --no_cold_trim:
#   the cache layer sends no trim to the cold FTL, so stale cold copies stay
#   valid until re-evicted (overwrite). Higher/steadier cold utilization ->
#   less WAF fluctuation than the trim-on runs.
#   LBA span sanity: inject reserve [0,3.6TB) + trace remap (~0.85TB) < 5.12TB
#   capacity, so the cold FTL keeps ~13% effective OP and GC stays functional.
set -u
cd "$(dirname "$0")"

TRACE=/home/sejun000/dwpd_hi_first14TB_writes.csv
CACHE=640000000000                 # 640 GB
COLD=5120000000000                 # 5.12 TB
SEG=2147483648                     # 2 GiB
MAW=524288
RESERVE=3600000000000              # 3.6 TB synthetic reserve
WARMUP=15393162788864              # 14 TiB host -> fill-then-inject gate
CACHE_LIMIT=10995116277760000      # non-binding; cold limit stops first
COLD_LIMIT=21990232555520          # 20 TiB cold writes
R=8.64

INJECT="--seq_inject_period 9 --seq_inject_frac 0.10 --cold_reserve_bytes $RESERVE --seq_inject_warmup_bytes $WARMUP"
COMMON="--rw_policy write-only --trace_format csv4col --cold_capacity $COLD \
        --segment_size $SEG --remap_lba --no_fill --loop_trace --no_cold_trim \
        --cache_write_size_limit $CACHE_LIMIT --cold_write_size_limit $COLD_LIMIT"

# --- REFLASH (GS_FINAL D=1) ---
TAG=dwpd_hi_coldlim20_notrim_REFLASH
echo "[launch] $TAG"
GS_DECISION_LOG="${TAG}.gsdec.log" \
stdbuf -oL ./cache_sim "$TRACE" "$CACHE" $COMMON $INJECT \
    --cache_policy LOG_GREEDY_COST_BENEFIT_10_GS_FINAL \
    --gs_decision_period_segs 1 \
    --periodic_ratio "$R" --util_step 0.02 \
    --moving_avg_type ewma --moving_avg_window "$MAW" \
    --waf_log_file "${TAG}.waf.log" --stat_log_file "${TAG}.stat" \
    > "${TAG}.run.log" 2>&1 &
echo "  pid=$!"
sleep 1

# --- LOG_FIFO baseline ---
TAG=dwpd_hi_coldlim20_notrim_LOGFIFO
echo "[launch] $TAG"
stdbuf -oL ./cache_sim "$TRACE" "$CACHE" $COMMON $INJECT \
    --cache_policy LOG_FIFO \
    --waf_log_file "${TAG}.waf.log" --stat_log_file "${TAG}.stat" \
    > "${TAG}.run.log" 2>&1 &
echo "  pid=$!"

echo "both launched. waiting..."
wait
echo "all done at $(date)"
