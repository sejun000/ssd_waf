#!/bin/bash
# Smoke test: replay variant. Reuses seg=16 gsdec log as the replay source.
set -u
POLICY=LOG_GREEDY_COST_BENEFIT_10_GS_REPLAY
MA=ewma
MAW=1572864
TRACE=/home/sejun000/alibaba_dwpd1to2_4x.trace
DEV=15000000000000
COLD=16050000000000
ALIGN=13079937024
CACHE=$(( ((DEV / 8 + ALIGN - 1) / ALIGN) * ALIGN ))
SCALE=2
R=8.64
RT=864
SUFFIX=gsdec864replay
P=16

# Replay source: original seg=16 gsdec log.
SRC_LOG=LOG_GREEDY_COST_BENEFIT_10_GS_us02_ewma_hl1572864_gsdec864_segs${P}_pr${RT}.gsdec.log

cd "$(dirname "$0")"
TS=$(date +%y%m%d_%H%M%S)
TAG="${POLICY}_us02_${MA}_hl${MAW}_${SUFFIX}_segs${P}"
STAT="${TAG}.stat_pr${RT}"
WAF="${TAG}_pr${RT}_${TS}.waf.log"
LOG="run_${TAG}_pr${RT}.log"
echo "segs=${P} ts=${TS} replay_from=${SRC_LOG}"
echo "-> ${STAT}"
GS_REPLAY_LOG="$SRC_LOG" ./cache_sim "$TRACE" "$CACHE" \
    --rw_policy write-only --trace_format csv \
    --cache_policy "$POLICY" \
    --cache_trace /mnt/nvme2n2/${TAG}_pr${RT}.trace \
    --cold_trace /mnt/nvme2n2/${TAG}_pr${RT}.cold.trace \
    --cold_capacity "$COLD" \
    --waf_log_file "$WAF" \
    --periodic_ratio "$R" --util_step 0.02 \
    --moving_avg_type "$MA" --moving_avg_window "$MAW" \
    --gs_decision_period_segs "$P" \
    --stat_log_file "$STAT" --scale "$SCALE" \
    > "$LOG" 2>&1
echo "done segs=${P} at $(date) -> log: $LOG"
