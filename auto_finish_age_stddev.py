#!/usr/bin/env python3
"""Detached finisher for the A_age_stddev_cdf.pdf refresh.

Discovers the freshest LOG_SEPBIT_FIFO / LOG_GREEDY_COST_BENEFIT_80 outputs
(strictly newer than a launch-time threshold so the old 20260304 files are
never picked), repoints the SepBIT/REFlash entries in plot_age_stddev.py, and
regenerates the PDF. Fail-safe: if fresh outputs are missing it aborts WITHOUT
touching the plot script.

Usage: python3 auto_finish_age_stddev.py <threshold_epoch>
"""
import glob, os, re, sys, subprocess

ROOT = "/home/sejun000/ssd_waf"
PLOT = os.path.join(ROOT, "plot_age_stddev.py")

threshold = float(sys.argv[1]) if len(sys.argv) > 1 else 0.0

def newest(pattern):
    cands = [f for f in glob.glob(os.path.join(ROOT, pattern))
             if os.path.getmtime(f) >= threshold - 60]
    return max(cands, key=os.path.getmtime) if cands else None

specs = {
    "sepbit_inv":   "LOG_SEPBIT_FIFO.inv_time_scatter.*.csv",
    "sepbit_stat":  "LOG_SEPBIT_FIFO.stat.log.*",
    "reflash_inv":  "LOG_GREEDY_COST_BENEFIT_80.inv_time_scatter.*.csv",
    "reflash_stat": "LOG_GREEDY_COST_BENEFIT_80.stat.log.*",
}
found = {k: newest(v) for k, v in specs.items()}
missing = [k for k, v in found.items() if v is None]
if missing:
    print(f"[auto_finish] ABORT: fresh outputs missing for {missing} "
          f"(threshold={threshold}); plot left untouched", flush=True)
    sys.exit(1)

base = {k: os.path.basename(v) for k, v in found.items()}
print("[auto_finish] using outputs:", flush=True)
for k, v in base.items():
    print(f"    {k:12s} {v}", flush=True)

with open(PLOT) as f:
    text = f.read()

# NB: LOG_GREEDY_COST_BENEFIT_COLD_80 (REFlash-CB) and LOG_GREEDY_80 (Greedy)
# do NOT contain the literal "LOG_GREEDY_COST_BENEFIT_80." substring, so these
# subs only touch the SepBIT/REFlash entries.
subs = [
    (r"LOG_SEPBIT_FIFO\.inv_time_scatter\.\d+_\d+\.csv", base["sepbit_inv"]),
    (r"LOG_SEPBIT_FIFO\.stat\.log\.\d+_\d+",             base["sepbit_stat"]),
    (r"LOG_GREEDY_COST_BENEFIT_80\.inv_time_scatter\.\d+_\d+\.csv", base["reflash_inv"]),
    (r"LOG_GREEDY_COST_BENEFIT_80\.stat\.log\.\d+_\d+",  base["reflash_stat"]),
]
for pat, repl in subs:
    text, n = re.subn(pat, repl, text)
    print(f"[auto_finish] sub {pat} -> {repl} ({n} hit)", flush=True)

with open(PLOT, "w") as f:
    f.write(text)

print("[auto_finish] regenerating A_age_stddev_cdf.pdf ...", flush=True)
r = subprocess.run([sys.executable, PLOT], cwd=ROOT)
print(f"[auto_finish] plot exit={r.returncode}", flush=True)
sys.exit(r.returncode)
