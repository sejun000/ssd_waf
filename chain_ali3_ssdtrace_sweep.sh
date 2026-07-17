#!/bin/bash
# Sequential REFLASH (or LOG_GREEDY_COST_BENEFIT_11) sweeps for Alibaba3 (dwpd01to1)
# then SSDtrace (scaled_4x). Each sweep generates dp.0.10 ~ dp.0.90 in
# /home/sejun000/ssd_waf/; we move them into a per-trace folder before the next run.
set -e

cd /home/sejun000/ssd_waf

POL=LOG_GREEDY_COST_BENEFIT_11

echo "[$(date)] === Ali3 sweep start (alibaba_dwpd01to1) ==="
sudo python3 ./blk_trace_analysis.py \
    --trace_format csv --rw_policy write-only \
    --cache_policy $POL \
    --valid_rate 0.10,0.90 \
    --periodic_ratio 8.64 --scale 2 \
    /home/sejun000/alibaba_dwpd01to1.trace \
    > sweep_ali3_dwpd01to1.log 2>&1
echo "[$(date)] === Ali3 sweep done ==="

mkdir -p ali3_dp
sudo mv dp.0.* ali3_dp/ 2>/dev/null || true
echo "[$(date)] moved $(ls ali3_dp | wc -l) files to ali3_dp/"

echo "[$(date)] === SSDtrace sweep start (ssdtrace_scaled_4x) ==="
sudo python3 ./blk_trace_analysis.py \
    --trace_format csv --rw_policy write-only \
    --cache_policy $POL \
    --valid_rate 0.10,0.90 \
    --periodic_ratio 8.64 --scale 2 \
    /home/sejun000/ssdtrace_scaled_4x.trace \
    > sweep_ssdtrace_scaled4x.log 2>&1
echo "[$(date)] === SSDtrace sweep done ==="

mkdir -p ssdtrace_dp
sudo mv dp.0.* ssdtrace_dp/ 2>/dev/null || true
echo "[$(date)] moved $(ls ssdtrace_dp | wc -l) files to ssdtrace_dp/"
echo "[$(date)] all sweeps complete."
