#!/bin/bash
# U_B (valid_rate) sweep driver — runs all trace sweeps SEQUENTIALLY (to avoid OOM), each into its OWN folder.
#
# Same config as chain_ali{1,2,3}_ssdtrace_sweep.sh:
#   policy   LOG_GREEDY_COST_BENEFIT_11
#   valid_rate 0.10 .. 0.90   (-> dp.0.10 .. dp.0.90, 81 files per trace)
#   scale 2,  periodic_ratio 8.64,  write-only, csv
#
# Difference vs the old chain scripts:
#   - each sweep runs with CWD = its own per-trace folder, so blk_trace_analysis.py's
#     "--stat_log_file dp.<vr>" lands directly inside that folder (no shared-CWD collision,
#     no post-hoc `mv dp.0.*`). This lets all sweeps run concurrently.
#   - cache_sim is symlinked into each folder so the hardcoded "./cache_sim" resolves.
#   - /mnt/nvme2n2 cache/cold dump scratch stays shared (write-only, never read back ->
#     does not affect dp.* stats; same as the original 10-way intra-sweep sharing).
set -u

REPO=/home/sejun000/ssd_waf
PY="$REPO/blk_trace_analysis.py"
BIN="$REPO/cache_sim"
POL=LOG_GREEDY_COST_BENEFIT_11

run_sweep() {
    local tag="$1" trace="$2"
    local dir="$REPO/$tag"
    mkdir -p "$dir"
    ln -sf "$BIN" "$dir/cache_sim"
    cd "$dir" || { echo "[ERR] cd $dir failed"; return 1; }
    echo "[$(date)] === $tag START  trace=$trace  cwd=$dir ==="
    python3 "$PY" \
        --trace_format csv --rw_policy write-only \
        --cache_policy "$POL" \
        --valid_rate 0.50,0.90 \
        --periodic_ratio 8.64 --scale 2 \
        "$trace" > "$dir/sweep_${tag}.log" 2>&1
    local rc=$? n
    n=$(ls "$dir"/dp.0.* 2>/dev/null | wc -l)
    echo "[$(date)] === $tag DONE   rc=$rc  dp_files=$n  ($dir) ==="
}

echo "[$(date)] ###### U_B sweep (parallel, per-folder) launching ######"

run_sweep ubsweep_ali1_dwpd2_5x    /home/sejun000/alibaba_dwpd2_5x.trace
run_sweep ubsweep_ali2_dwpd1to2_4x /home/sejun000/alibaba_dwpd1to2_4x.trace
run_sweep ubsweep_ali3_dwpd01to1   /home/sejun000/alibaba_dwpd01to1.trace
run_sweep ubsweep_ssdtrace_4x      /home/sejun000/ssdtrace_scaled_4x.trace

echo "[$(date)] ###### ALL U_B SWEEPS COMPLETE ######"
for tag in ubsweep_ali1_dwpd2_5x ubsweep_ali2_dwpd1to2_4x ubsweep_ali3_dwpd01to1 ubsweep_ssdtrace_4x; do
    echo "  $tag : $(ls "$REPO/$tag"/dp.0.* 2>/dev/null | wc -l) dp files"
done
