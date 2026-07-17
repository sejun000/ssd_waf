#!/bin/bash
# Phase 1 candidate tracker sweep: segs in {1,2,4,8} x r in {2.88, 8.64}.
# Tag uses gsv4p1 (Phase 1 on top of v4 anchor).
set -u
POLICY=LOG_GREEDY_COST_BENEFIT_10_GS
MA_TYPE=ewma
MA_WINDOW=1572864
TRACE=/home/sejun000/alibaba_dwpd1to2_4x.trace
DEVICE_SIZE=15000000000000
COLD_CAP=16050000000000
ALIGN=13079937024
CACHE=$(( ((DEVICE_SIZE / 8 + ALIGN - 1) / ALIGN) * ALIGN ))
SCALE=2

R_VALS=(2.88 8.64)
R_TAGS=(288 864)

cd "$(dirname "$0")"
echo "GS Phase 1 sweep start at $(date)"

for RT_IDX in 0 1; do
    R=${R_VALS[$RT_IDX]}
    RT=${R_TAGS[$RT_IDX]}
    TS=$(date +%y%m%d_%H%M%S)
    echo "[r=${R}] launching 4 segs at $(date)"
    for P in 1 2 4 8; do
        TAG="${POLICY}_us02_${MA_TYPE}_hl${MA_WINDOW}_gsv4p1_segs${P}"
        STAT="${TAG}.stat_pr${RT}"
        WAF="${TAG}_pr${RT}_${TS}.waf.log"
        LOG="run_${TAG}_pr${RT}.log"
        echo "  segs=${P} -> ${STAT}"
        nohup ./cache_sim "$TRACE" "$CACHE" \
            --rw_policy write-only \
            --trace_format csv \
            --cache_policy "$POLICY" \
            --cache_trace /mnt/nvme2n2/${TAG}_pr${RT}.trace \
            --cold_trace /mnt/nvme2n2/${TAG}_pr${RT}.cold.trace \
            --cold_capacity "$COLD_CAP" \
            --waf_log_file "$WAF" \
            --periodic_ratio "$R" \
            --util_step 0.02 \
            --moving_avg_type "$MA_TYPE" \
            --moving_avg_window "$MA_WINDOW" \
            --gs_decision_period_segs "$P" \
            --stat_log_file "$STAT" \
            --scale "$SCALE" \
            > "$LOG" 2>&1 &
    done
    wait
    echo "[r=${R}] done at $(date)"
done
echo "GS Phase 1 sweep all done at $(date)"
