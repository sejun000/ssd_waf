#!/bin/bash
# Direct-EWMA (skip-on-same) GS_FINAL — marg2 — at r in {2.88, 8.64} x dN {1,2,4,8,16}.
# Same alibaba setup as the old marg runs; tag = marg2 to keep old results intact.
cd /home/sejun000/ssd_waf
TRACE=/home/sejun000/alibaba_dwpd1to2_4x.trace
CACHE=1883510931456
LIMIT=15393162788864      # 14 TiB host write, single pass
CAP=6
MIN_FREE=350              # GB available required before launching another
declare -A RT=( [288]=2.88 [864]=8.64 )

launch() {
  local d=$1 rtag=$2 R=$3
  local TAG=LOG_GREEDY_COST_BENEFIT_10_GS_FINAL_ewma_hl1572864_marg2_gsD${d}_r${rtag}
  GS_DECISION_LOG=${TAG}.gsdec.log nohup ./cache_sim "$TRACE" "$CACHE" \
    --rw_policy write-only --trace_format csv \
    --cache_policy LOG_GREEDY_COST_BENEFIT_10_GS_FINAL \
    --cold_capacity 16050000000000 --scale 2 \
    --periodic_ratio "$R" --util_step 0.02 \
    --moving_avg_type ewma --moving_avg_window 1572864 \
    --gs_decision_period_segs "$d" \
    --cache_write_size_limit "$LIMIT" \
    --waf_log_file ${TAG}.waf.log --stat_log_file ${TAG}.stat \
    > run_${TAG}.log 2>&1 &
  echo "[launch] $TAG pid=$!"
}

for rtag in 288 864; do
  R=${RT[$rtag]}
  for d in 1 2 4 8 16; do
    while :; do
      running=$(pgrep -fc "alibaba_dwpd1to2_4x")
      avail=$(free -g | awk 'NR==2{print $7}')
      if [ "${running:-0}" -lt "$CAP" ] && [ "${avail:-0}" -ge "$MIN_FREE" ]; then break; fi
      sleep 30
    done
    launch "$d" "$rtag" "$R"
    sleep 30
  done
done
wait
echo "ALL DONE $(date)" > marg2_2r.DONE
