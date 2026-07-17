#!/bin/bash
# Run GS (tick-based update_ghost_compacted_blocks_sum) for seg ∈ {1,2,4,8,16,32}.
# Per-seg half-life = 2 × segs × segment_size_blocks  (segment_size_blocks = 1572864).
# Usage: ./run_gs_tickfix_all.sh <R>
set -u
POLICY=LOG_GREEDY_COST_BENEFIT_10_GS
MA=ewma
SEG_BLOCKS=1572864
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
    local P=$1
    local MAW=$(( 2 * P * SEG_BLOCKS ))
    local TS=$(date +%y%m%d_%H%M%S)
    local SUFFIX=gsdec${RT}hl2xseg${P}tickfix
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

for P in 1 2 4 8 16 32; do run_one "$P"; done

echo "all 6 launched (R=$R, hl = 2*P*${SEG_BLOCKS})"
