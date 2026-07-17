#!/bin/bash
# GS dec=1seg, per-compaction ghost_compacted_blocks_sum_ (monotone +=).
# RHS = Gud - Gu restored. hl=4seg, delta ∈ {1,2,4,8,16,32}.
# Usage: ./run_gs_dec1_v2_all.sh <R>
set -u
POLICY=LOG_GREEDY_COST_BENEFIT_10_GS
MA=ewma
MAW=6291456
TRACE=/home/sejun000/alibaba_dwpd1to2_4x.trace
DEV=15000000000000
COLD=16050000000000
ALIGN=13079937024
CACHE=$(( ((DEV / 8 + ALIGN - 1) / ALIGN) * ALIGN ))
SCALE=2
R=${1:-8.64}
RT=$(echo "$R" | tr -d '.')

cd "$(dirname "$0")"

run_one() {
    local D=$1
    local TS=$(date +%y%m%d_%H%M%S)
    local SUFFIX=gsdec${RT}dec1v2d${D}
    local TAG="${POLICY}_us02_${MA}_hl${MAW}_${SUFFIX}_d${D}"
    local STAT="${TAG}.stat_pr${RT}"
    local WAF="${TAG}_pr${RT}_${TS}.waf.log"
    local LOG="run_${TAG}_pr${RT}.log"
    local GSDEC="${TAG}_pr${RT}.gsdec.log"
    echo "[start delta=${D} r=${R}] gsdec=$GSDEC"
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

for D in 1 2 4 8 16 32; do run_one "$D"; done
echo "all 6 launched (R=$R)"
