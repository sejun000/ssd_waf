#!/bin/bash
# GS_FINAL with the new invrate rule (Gud < invrate_sum·r → RAISE).
# Replicates the recent GS_FINAL baseline (D=1, hl=1 seg) so the ONLY change is
# the decision rule. cache_trace/cold_trace dumps omitted (not needed here; keeps
# the root fs from filling). GS_DECISION_LOG on → vic_v_wt / invrate_sum columns.
# Usage: ./run_gsfinal_invrate.sh        (launches r=2.88 and r=8.64, D=1, scale 2)
set -u
cd "$(dirname "$0")"

POLICY=LOG_GREEDY_COST_BENEFIT_10_GS_FINAL
MA=ewma
MAW=1572864                 # ratio EWMA half-life = 1 × seg (recent baseline)
D=1                         # gs_decision_period_segs (REFLASH-equiv baseline)
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
    local TAG="${POLICY}_invrate_nocap_hl${MAW}_d${D}_pr${RT}"
    local STAT="${TAG}.stat"
    local WAF="${TAG}_${TS}.waf.log"
    local LOG="run_${TAG}.log"
    local GSDEC="${TAG}.gsdec.log"
    echo "[launch r=${R}] tag=${TAG}"
    GS_DECISION_LOG="$GSDEC" nohup ./cache_sim "$TRACE" "$CACHE" \
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
echo "both launched at $TS (D=$D, hl=$MAW, scale=$SCALE, cache=$CACHE)"
