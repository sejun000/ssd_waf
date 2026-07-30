#!/bin/bash
# Backend(capacity-tier) GC comparison: LOG_FIFO vs REFLASH under identical load.
#   Same config as run_dwpd_hi_fill14_coldlim.sh: fill 14TiB(trace) -> trace + 10%
#   bg seq inject, stop when COLD writes hit 20TiB (~4x cold capacity) so the cold
#   FTL runs steady GC. Compare cold GC volume via waf.log col4-col3 (nand-host)
#   at equal col1/col3 progress; cold WAF = col4/col3. (LOG_FIFO stat ftl columns
#   are fake ftl_nand=ftl_host — do not use them.)
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
        --segment_size $SEG --remap_lba --no_fill --loop_trace \
        --cache_write_size_limit $CACHE_LIMIT --cold_write_size_limit $COLD_LIMIT"

# --- REFLASH (GS_FINAL D=1) ---
TAG=dwpd_hi_coldlim20_blk2g_REFLASH
echo "[launch] $TAG"
GS_DECISION_LOG="${TAG}.gsdec.log" \
stdbuf -oL ./cache_sim_blk2g "$TRACE" "$CACHE" $COMMON $INJECT \
    --cache_policy LOG_GREEDY_COST_BENEFIT_10_GS_FINAL \
    --gs_decision_period_segs 1 \
    --periodic_ratio "$R" --util_step 0.02 \
    --moving_avg_type ewma --moving_avg_window "$MAW" \
    --waf_log_file "${TAG}.waf.log" --stat_log_file "${TAG}.stat" \
    > "${TAG}.run.log" 2>&1 &
echo "  pid=$!"
sleep 1

# --- LOG_FIFO baseline ---
TAG=dwpd_hi_coldlim20_blk2g_LOGFIFO
echo "[launch] $TAG"
stdbuf -oL ./cache_sim_blk2g "$TRACE" "$CACHE" $COMMON $INJECT \
    --cache_policy LOG_FIFO \
    --waf_log_file "${TAG}.waf.log" --stat_log_file "${TAG}.stat" \
    > "${TAG}.run.log" 2>&1 &
echo "  pid=$!"

echo "both launched. waiting..."
wait
echo "all done at $(date)"
