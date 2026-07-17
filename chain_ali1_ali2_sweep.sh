#!/bin/bash
# Wait for chain_ali3_ssdtrace_sweep.sh to finish, then run Ali1 + Ali2 sweeps
# with the same LOG_GREEDY_COST_BENEFIT_11 / valid_rate 0.10~0.90 / scale=2 / pr=8.64 config.
set -e

cd /home/sejun000/ssd_waf
POL=LOG_GREEDY_COST_BENEFIT_11

echo "[$(date)] waiting for chain_ali3_ssdtrace_sweep.sh to finish..."
while pgrep -f chain_ali3_ssdtrace_sweep.sh > /dev/null; do
    sleep 60
done
echo "[$(date)] previous chain finished. starting Ali1 sweep."

echo "[$(date)] === Ali1 sweep start (alibaba_dwpd2_5x) ==="
sudo python3 ./blk_trace_analysis.py \
    --trace_format csv --rw_policy write-only \
    --cache_policy $POL \
    --valid_rate 0.10,0.90 \
    --periodic_ratio 8.64 --scale 2 \
    /home/sejun000/alibaba_dwpd2_5x.trace \
    > sweep_ali1_dwpd2_5x.log 2>&1
echo "[$(date)] === Ali1 sweep done ==="

mkdir -p ali1_dp_cb11
sudo mv dp.0.* ali1_dp_cb11/ 2>/dev/null || true
echo "[$(date)] moved $(ls ali1_dp_cb11 | wc -l) files to ali1_dp_cb11/"

echo "[$(date)] === Ali2 sweep start (alibaba_dwpd1to2_4x) ==="
sudo python3 ./blk_trace_analysis.py \
    --trace_format csv --rw_policy write-only \
    --cache_policy $POL \
    --valid_rate 0.10,0.90 \
    --periodic_ratio 8.64 --scale 2 \
    /home/sejun000/alibaba_dwpd1to2_4x.trace \
    > sweep_ali2_dwpd1to2.log 2>&1
echo "[$(date)] === Ali2 sweep done ==="

mkdir -p ali2_dp_cb11
sudo mv dp.0.* ali2_dp_cb11/ 2>/dev/null || true
echo "[$(date)] moved $(ls ali2_dp_cb11 | wc -l) files to ali2_dp_cb11/"
echo "[$(date)] Ali1 + Ali2 sweeps complete."
