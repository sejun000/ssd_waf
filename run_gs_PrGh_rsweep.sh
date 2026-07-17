#!/bin/bash
# PrGh r-sweep at fixed D=1.
# Usage: ./run_gs_PrGh_rsweep.sh   (launches r in 5.76, 11.52, 14.40 — 2.88 & 8.64 already done)
set -u
POLICY=LOG_GREEDY_COST_BENEFIT_10_GS_FINAL
MA=ewma
MAW=1572864
TRACE=/home/sejun000/alibaba_dwpd1to2_4x.trace
DEV=15000000000000
COLD=16050000000000
ALIGN=13079937024
CACHE=$(( ((DEV / 8 + ALIGN - 1) / ALIGN) * ALIGN ))
SCALE=2
D=1

cd "$(dirname "$0")"

run_one() {
    local R=$1
    local RT=$(echo "$R" | tr -d '.')
    local TS=$(date +%y%m%d_%H%M%S)
    local SUFFIX=gsdec${RT}PrGhD${D}
    local TAG="${POLICY}_us02_${MA}_hl${MAW}_${SUFFIX}_d${D}"
    local STAT="${TAG}.stat_pr${RT}"
    local WAF="${TAG}_pr${RT}_${TS}.waf.log"
    local LOG="run_${TAG}_pr${RT}.log"
    local GSDEC="${TAG}_pr${RT}.gsdec.log"
    echo "[start D=${D} r=${R}] gsdec=$GSDEC"
    GS_DECISION_LOG="$GSDEC" ./cache_sim "$TRACE" "$CACHE" \
        --rw_policy write-only --trace_format csv \
        --cache_policy "$POLICY" \
        --cache_trace /mnt/nvme2n2/${TAG}_pr${RT}.trace \
        --cold_trace  /mnt/nvme2n2/${TAG}_pr${RT}.cold.trace \
        --cold_capacity "$COLD" \
        --waf_log_file "$WAF" \
        --periodic_ratio "$R" --util_step 0.02 \
        --moving_avg_type "$MA" --moving_avg_window "$MAW" \
        --gs_decision_period_segs "$D" \
        --stat_log_file "$STAT" --scale "$SCALE" \
        > "$LOG" 2>&1 &
    echo "  pid=$!"
    sleep 1
}

for R in 5.76 11.52 14.40; do run_one "$R"; done
echo "3 r-sweep launched (D=1, PrGh)"
