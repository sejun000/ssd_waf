#!/usr/bin/env python3
"""Detached finisher for the REFlash-CB (sqrt) refresh of A_age_stddev_cdf.pdf.

Repoints ONLY the REFlash-CB entries (LOG_GREEDY_COST_BENEFIT_COLD_80) in
plot_age_stddev.py to the freshest outputs (strictly newer than a launch-time
threshold) and regenerates the PDF. Fail-safe: aborts WITHOUT touching the plot
if fresh outputs are missing.

Usage: python3 auto_finish_reflashcb.py <threshold_epoch>
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
    "cb_inv":  "LOG_GREEDY_COST_BENEFIT_COLD_80.inv_time_scatter.*.csv",
    "cb_stat": "LOG_GREEDY_COST_BENEFIT_COLD_80.stat.log.*",
}
found = {k: newest(v) for k, v in specs.items()}
missing = [k for k, v in found.items() if v is None]
if missing:
    print(f"[cb_finish] ABORT: fresh outputs missing for {missing} "
          f"(threshold={threshold}); plot left untouched", flush=True)
    sys.exit(1)

base = {k: os.path.basename(v) for k, v in found.items()}
print("[cb_finish] using outputs:", flush=True)
for k, v in base.items():
    print(f"    {k:8s} {v}", flush=True)

with open(PLOT) as f:
    text = f.read()

# COLD_80 substring does NOT appear in LOG_GREEDY_COST_BENEFIT_80 (REFlash) nor
# in LOG_GREEDY_COST_BENEFIT_80_COLD (different order), so only REFlash-CB moves.
subs = [
    (r"LOG_GREEDY_COST_BENEFIT_COLD_80\.inv_time_scatter\.\d+_\d+\.csv", base["cb_inv"]),
    (r"LOG_GREEDY_COST_BENEFIT_COLD_80\.stat\.log\.\d+_\d+",            base["cb_stat"]),
]
for pat, repl in subs:
    text, n = re.subn(pat, repl, text)
    print(f"[cb_finish] sub {pat} -> {repl} ({n} hit)", flush=True)

with open(PLOT, "w") as f:
    f.write(text)

print("[cb_finish] regenerating A_age_stddev_cdf.pdf ...", flush=True)
r = subprocess.run([sys.executable, PLOT], cwd=ROOT)
print(f"[cb_finish] plot exit={r.returncode}", flush=True)
sys.exit(r.returncode)
