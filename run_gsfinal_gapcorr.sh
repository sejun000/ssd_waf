#!/bin/bash
# GS_FINAL invrate rule + invalidate-gap correction (GUD_GAP_CORRECT=1).
# Same config as run_gsfinal_invrate.sh nocap baseline; ONLY change is the env
# toggle that makes get_kth discount each victim's valid by invalidate_rate·H.
# OFF baseline = the existing *_invrate_nocap_*_d1_pr{288,864} logs.
# Usage: ./run_gsfinal_gapcorr.sh        (launches r=2.88 and r=8.64, D=1, scale 2)
set -u
cd "$(dirname "$0")"

POLICY=LOG_GREEDY_COST_BENEFIT_10_GS_FINAL
MA=ewma
MAW=1572864
D=1
TRACE=/home/sejun000/alibaba_dwpd1to2_4x.trace
DEV=15000000000000
COLD=16050000000000
ALIGN=13079937024
CACHE=$(( ((DEV / 8 + ALIGN - 1) / ALIGN) * ALIGN ))   # = 1883510931456
SCALE=2
TS=$(date +%y%m%d_%H%M%S)

run_one() {
    local R=$1
    local RT=$(echo "$R" | tr -d '.')
    local TAG="${POLICY}_gapcorr_hl${MAW}_d${D}_pr${RT}"
    local STAT="${TAG}.stat"
    local WAF="${TAG}_${TS}.waf.log"
    local LOG="run_${TAG}.log"
    local GSDEC="${TAG}.gsdec.log"
    echo "[launch r=${R}] tag=${TAG}  (GUD_GAP_CORRECT=1)"
    GUD_GAP_CORRECT=1 GS_DECISION_LOG="$GSDEC" nohup ./cache_sim "$TRACE" "$CACHE" \
        --rw_policy write-only --trace_format csv \
        --cache_policy "$POLICY" \
        --cold_capacity "$COLD" \
        --waf_log_file "$WAF" \
        --periodic_ratio "$R" --util_step 0.02 \
        --moving_avg_type "$MA" --moving_avg_window "$MAW" \
        --gs_decision_period_segs "$D" \
        --stat_log_file "$STAT" --scale "$SCALE" \
        > "$LOG" 2>&1 &
    echo "  pid=$!  log=$LOG  gsdec=$GSDEC  waf=$WAF  stat=$STAT"
}

run_one 2.88
run_one 8.64
echo "both launched at $TS (GUD_GAP_CORRECT=1, D=$D, hl=$MAW, scale=$SCALE)"
