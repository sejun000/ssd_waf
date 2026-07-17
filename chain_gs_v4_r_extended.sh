#!/bin/bash
# GS v4 extended r sweep: r in {2.88, 5.76, 8.64, 11.52, 14.4} parallel
# (5 cache_sim per batch), segs in {1,2,4,8,16,32,64,128} serialized.
# File suffix uses r*100 integer (pr288, pr576, pr864, pr1152, pr1440) so
# filenames sort cleanly.
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

R_VALS=(2.88 5.76 8.64 11.52 14.4)
R_TAGS=(288 576 864 1152 1440)

cd "$(dirname "$0")"
echo "GS v4 r-extended sweep start at $(date)"

for P in 1 2 4 8 16 32 64 128; do
    TS=$(date +%y%m%d_%H%M%S)
    echo "[segs=${P}] launching 5 r values at $(date)"
    for i in 0 1 2 3 4; do
        R=${R_VALS[$i]}
        RT=${R_TAGS[$i]}
        TAG="${POLICY}_us02_${MA_TYPE}_hl${MA_WINDOW}_gsv4_segs${P}"
        STAT="${TAG}.stat_pr${RT}"
        WAF="${TAG}_pr${RT}_${TS}.waf.log"
        LOG="run_${TAG}_pr${RT}.log"
        echo "  r=${R} (pr${RT}) -> ${STAT}"
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
    echo "[segs=${P}] done at $(date)"
done
echo "GS v4 r-extended sweep all done at $(date)"
