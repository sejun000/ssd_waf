#!/bin/bash
# GS decision-trace logging run. Same as v4 ff4 sweep but with GS_DECISION_LOG
# enabled so periodic_ghost_delta_gc_sum dumps a per-decision row.
# segs 1,2,4,8 parallel, r=8.64.
set -u
POLICY=LOG_GREEDY_COST_BENEFIT_10_GS
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
SUFFIX=gsdec864

cd "$(dirname "$0")"
TS=$(date +%y%m%d_%H%M%S)
echo "GS decision-trace sweep (segs 1,2,4,8) r=${R} ts=${TS}"
for P in 1 2 4 8; do
    TAG="${POLICY}_us02_${MA}_hl${MAW}_${SUFFIX}_segs${P}"
    STAT="${TAG}.stat_pr${RT}"
    WAF="${TAG}_pr${RT}_${TS}.waf.log"
    LOG="run_${TAG}_pr${RT}.log"
    GSDEC="${TAG}_pr${RT}.gsdec.log"
    echo "  segs=${P} -> ${STAT} | ${GSDEC}"
    GS_DECISION_LOG="$GSDEC" nohup ./cache_sim "$TRACE" "$CACHE" \
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
        > "$LOG" 2>&1 &
    echo "    pid=$!"
done
wait
echo "done segs 1,2,4,8 r=${R} at $(date)"
