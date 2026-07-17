#!/bin/bash
# dwpd_hi: "fill cold with 14TB trace first, THEN trace + 10% background seq writes".
#   - Single continuous run (state isn't persisted across processes, so a separate
#     "fill" process can't hand its cold contents to a second process).
#   - Phase 1 (host 0 -> 14 TiB): trace only, seq injection OFF -> cold fills naturally
#     via normal eviction. compactor heap stays warm (unlike --prefill_cold_device,
#     which empties it and degenerates REFLASH to FIFO).
#   - Phase 2 (host >= 14 TiB): trace + 10% background sequential injection into reserve.
#   Geometry = validated run_dwpd_himid_512_inj10.sh (cache640/cold5.12/seg2GiB/reserve3.6).
#   New knob: --seq_inject_warmup_bytes gates injection start.
set -u
cd "$(dirname "$0")"

TRACE=/home/sejun000/dwpd_hi_first14TB_writes.csv
CACHE=640000000000                 # 640 GB
COLD=5120000000000                 # 5.12 TB
SEG=2147483648                     # 2 GiB
MAW=524288                         # 1 seg in 4K blocks
RESERVE=3600000000000              # 3.6 TB synthetic reserve [0,R)
WARMUP=15393162788864              # 14 TiB host writes -> fill-then-inject gate (= ~1 trace pass)
CACHE_LIMIT=30786325577728         # 28 TiB host stop = fill(14) + measure(14)
COLD_LIMIT=54975581388800          # 50 TiB cold safety (not hit within 28 TiB host)
R=8.64
POLICY=LOG_GREEDY_COST_BENEFIT_10_GS_FINAL   # REFLASH

TAG="dwpd_hi_fill14_inj10_r864"
INJECT="--seq_inject_period 9 --seq_inject_frac 0.10 --cold_reserve_bytes $RESERVE --seq_inject_warmup_bytes $WARMUP"

echo "[launch] $TAG  trace=$TRACE  (phase1 fill 14TiB, then inject 10%)"
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
echo "[done] $TAG  exit=$?  -> ${TAG}.stat / ${TAG}.run.log"
