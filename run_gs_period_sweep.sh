#!/bin/bash
# GS sweep at a single decision-period value, r in {2,4,6,8,10} 5-parallel.
# Usage: run_gs_period_sweep.sh <PERIOD_SEGS>
set -u
PERIOD_SEGS="${1:?PERIOD_SEGS required}"

POLICY=LOG_GREEDY_COST_BENEFIT_10_GS
US=0.02     # ignored by GS (auto-derived from PERIOD_SEGS), kept for tag consistency
MA_TYPE=ewma
MA_WINDOW=1572864    # GS best half-life from prior sweep
TAG="${POLICY}_us02_${MA_TYPE}_hl${MA_WINDOW}_segs${PERIOD_SEGS}"

TRACE=/home/sejun000/alibaba_dwpd1to2_4x.trace
DEVICE_SIZE=15000000000000
COLD_CAP=16050000000000
ALIGN=13079937024
CACHE=$(( ((DEVICE_SIZE / 8 + ALIGN - 1) / ALIGN) * ALIGN ))
SCALE=2

cd "$(dirname "$0")"
TS=$(date +%y%m%d_%H%M%S)
echo "Policy=${POLICY}  period_segs=${PERIOD_SEGS}  ma=${MA_TYPE} win=${MA_WINDOW}  tag=${TAG}  cache=${CACHE}  scale=${SCALE}  ts=${TS}"

for r in 2 4 6 8 10; do
    STAT="${TAG}.stat_pr${r}"
    WAF="${TAG}_pr${r}_${TS}.waf.log"
    LOG="run_${TAG}_pr${r}.log"
    echo "Launching r=${r} -> stat=${STAT}"
    nohup ./cache_sim "$TRACE" "$CACHE" \
        --rw_policy write-only \
        --trace_format csv \
        --cache_policy "$POLICY" \
        --cache_trace /mnt/nvme2n2/${TAG}_pr${r}.trace \
        --cold_trace /mnt/nvme2n2/${TAG}_pr${r}.cold.trace \
        --cold_capacity "$COLD_CAP" \
        --waf_log_file "$WAF" \
        --periodic_ratio "$r" \
        --util_step "$US" \
        --moving_avg_type "$MA_TYPE" \
        --moving_avg_window "$MA_WINDOW" \
        --gs_decision_period_segs "$PERIOD_SEGS" \
        --stat_log_file "$STAT" \
        --scale "$SCALE" \
        > "$LOG" 2>&1 &
    echo "  pid=$!"
done
echo "All 5 ${TAG} launched. Waiting..."
wait
echo "${TAG} sweep done at $(date)"
