#!/bin/bash
# Like run_gs_fixed_all.sh but with configurable MAW (half-life).
# Usage: ./run_gs_fixed_hl.sh <R> <MAW>
set -u
POLICY=LOG_GREEDY_COST_BENEFIT_10_GS
MA=ewma
TRACE=/home/sejun000/alibaba_dwpd1to2_4x.trace
DEV=15000000000000
COLD=16050000000000
ALIGN=13079937024
CACHE=$(( ((DEV / 8 + ALIGN - 1) / ALIGN) * ALIGN ))
SCALE=2
R=${1:-8.64}
MAW=${2:-1572864}
RT=$(echo "$R" | tr -d '.')
SUFFIX=gsdec${RT}hl${MAW}fix

cd "$(dirname "$0")"

run_one() {
    local P=$1
    local TS=$(date +%y%m%d_%H%M%S)
    local TAG="${POLICY}_us02_${MA}_hl${MAW}_${SUFFIX}_segs${P}"
    local STAT="${TAG}.stat_pr${RT}"
    local WAF="${TAG}_pr${RT}_${TS}.waf.log"
    local LOG="run_${TAG}_pr${RT}.log"
    local GSDEC="${TAG}_pr${RT}.gsdec.log"
    echo "[start segs=${P} r=${R} hl=${MAW}] gsdec=$GSDEC"
    GS_DECISION_LOG="$GSDEC" ./cache_sim "$TRACE" "$CACHE" \
        --rw_policy write-only --trace_format csv \
        --cache_policy "$POLICY" \
        --cache_trace /mnt/nvme2n2/${TAG}_pr${RT}.trace \
        --cold_trace  /mnt/nvme2n2/${TAG}_pr${RT}.cold.trace \
        --cold_capacity "$COLD" \
        --waf_log_file "$WAF" \
        --periodic_ratio "$R" --util_step 0.02 \
        --moving_avg_type "$MA" --moving_avg_window "$MAW" \
        --gs_decision_period_segs "$P" \
        --stat_log_file "$STAT" --scale "$SCALE" \
        > "$LOG" 2>&1 &
    echo "  pid=$!"
    sleep 1
}

for P in 1 2 4 8 16; do run_one "$P"; done

echo "all 5 launched (R=$R, MAW=$MAW)"
