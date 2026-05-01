#!/usr/bin/env python3
"""NoDaP hierarchical grid search orchestrator.

Round logic (paper §3.1, MiDAS-style hierarchical split):
  Round 1 (N=2): try 10 G_1 boundaries [base * 1.5^i for i=0..9], pick min-WAF.
  Round 2 (N=3): keep G_1, try 10 G_2 boundaries above G_1 best, pick min-WAF.
  ...
  Stop when WAF reduction < 5%.

Each round launches `--parallel` cache_sim processes concurrently.
"""
import argparse
import os
import re
import subprocess
import sys
import time
from pathlib import Path

TRACE = "/home/sejun000/workloads/logs/cache_io_fio_ycsb_70_20260401_033750.trace"
CACHE_SIZE = 130_799_370_240        # 130 GB
COLD_CAPACITY = 1_070_000_000_000   # 1.07 TB

OUT_DIR = Path("/home/sejun000/ssd_waf/nodap_grid_runs")

COMP_RE = re.compile(r" compacted_blocks: (\d+) ")
WS_RE   = re.compile(r" write_size_to_cache: (\d+) ")
RENAME_RE = re.compile(r"stat log renamed to: (\S+)")


def find_real_stat_log(stdout_path: Path):
    """cache_sim ignores --stat_log_file and uses an auto-timestamped path.
    The actual path is printed in stdout: 'stat log renamed to: <name>'."""
    if not stdout_path.exists():
        return None
    actual = None
    with stdout_path.open() as f:
        for line in f:
            m = RENAME_RE.search(line)
            if m:
                actual = m.group(1)
    if actual:
        return Path("/home/sejun000/ssd_waf") / actual
    return None


def parse_waf(stdout_path: Path):
    """Return WAF (host+gc)/host from the LAST stat-log entry, or None on fail."""
    real = find_real_stat_log(stdout_path)
    if real is None or not real.exists():
        return None
    compacted = None
    write_size = None
    with real.open() as f:
        for line in f:
            mc = COMP_RE.search(line)
            mw = WS_RE.search(line)
            if mc and mw:
                compacted = int(mc.group(1))
                write_size = int(mw.group(1))
    if compacted is None or write_size is None or write_size == 0:
        return None
    host_blocks = write_size // 4096
    return (host_blocks + compacted) / host_blocks


def launch_run(run_id: str, bir_list):
    """Launch one cache_sim with NODAP_BIR=<bir_list>. Returns Popen."""
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    bir_str = ",".join(str(v) if v != float('inf') else "inf" for v in bir_list)
    waf_log = OUT_DIR / f"{run_id}.waf.log"
    stat_log = OUT_DIR / f"{run_id}.stat.log"
    stdout_log = OUT_DIR / f"{run_id}.stdout"

    # Skip --cache_trace/--cold_trace to avoid disk overhead during grid search.
    # No --fill: prefill breaks NoDaP oracle (pre-pass timestamps don't match
    # log_cache_timestamp when prefill consumes oracle entries).
    cmd = [
        "./cache_sim", TRACE, str(CACHE_SIZE),
        "--rw_policy", "all",
        "--trace_format", "blkparse_csv",
        "--cache_policy", "LOG_NODAP_88",
        "--cold_capacity", str(COLD_CAPACITY),
        "--waf_log_file", str(waf_log),
        "--stat_log_file", str(stat_log),
        "--periodic_ratio", "2.88",
    ]
    env = os.environ.copy()
    env["NODAP_BIR"] = bir_str
    f = open(stdout_log, "w")
    p = subprocess.Popen(cmd, env=env, stdout=f, stderr=subprocess.STDOUT,
                         cwd="/home/sejun000/ssd_waf")
    return p, stat_log, bir_str


def candidates_for_round(prev_boundaries, base_low, base_high, n=10, factor=2.0):
    """Return n BIR candidates between base_low and base_high (geometric step)."""
    cand = []
    v = float(base_low)
    for _ in range(n):
        if v > base_high:
            break
        cand.append(int(v))
        v *= factor
    return cand


def round_n(round_idx, fixed_boundaries, low, high, parallel, factor):
    """Run one round: insert a new boundary between `low` and `high`,
    keeping `fixed_boundaries` (list, ascending). Returns (best_boundary,
    best_waf, all_results).
    """
    candidates = candidates_for_round(fixed_boundaries, low, high, n=parallel, factor=factor)
    if not candidates:
        return None, None, []

    print(f"\n=== Round {round_idx} (N={len(fixed_boundaries)+2}) ===")
    print(f"fixed boundaries: {fixed_boundaries}")
    print(f"candidates: {candidates}")

    procs = []
    for i, c in enumerate(candidates):
        full_bir = sorted(fixed_boundaries + [c]) + [float('inf')]
        run_id = f"r{round_idx}_c{c}"
        p, stat_log, bir_str = launch_run(run_id, full_bir)
        stdout_path = OUT_DIR / f"{run_id}.stdout"
        procs.append((c, p, stdout_path, bir_str, run_id))
        print(f"  launched {run_id}: BIR={bir_str}", flush=True)
        time.sleep(1)  # avoid filesystem races on launch

    # Wait
    print("waiting for runs to complete...", flush=True)
    for c, p, stdout_path, bir_str, run_id in procs:
        p.wait()
        print(f"  {run_id} done (rc={p.returncode})", flush=True)

    # Parse
    results = []
    for c, p, stdout_path, bir_str, run_id in procs:
        waf = parse_waf(stdout_path)
        results.append((c, waf, run_id))
        print(f"  {run_id}: BIR={bir_str}, WAF={waf}", flush=True)

    valid = [(c, w, rid) for c, w, rid in results if w is not None]
    if not valid:
        return None, None, results
    valid.sort(key=lambda t: t[1])
    best_c, best_waf, best_rid = valid[0]
    print(f"BEST: c={best_c}, WAF={best_waf} ({best_rid})")
    return best_c, best_waf, results


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--parallel", type=int, default=10)
    ap.add_argument("--max_rounds", type=int, default=6)
    ap.add_argument("--min_improvement", type=float, default=0.05,
                    help="stop when WAF improvement < this fraction")
    ap.add_argument("--init_low", type=int, default=1000)
    ap.add_argument("--init_high", type=int, default=512_000)  # 2x step, 10 candidates: 1K, 2K, 4K, ..., 512K
    ap.add_argument("--factor", type=float, default=2.0)
    ap.add_argument("--initial_boundaries", type=str, default="",
                    help="comma-separated already-fixed boundaries to start from (skip round 1)")
    ap.add_argument("--start_round", type=int, default=1)
    args = ap.parse_args()

    # Round 1: N=2 (one boundary). Search candidates from init_low to init_high.
    fixed = []
    if args.initial_boundaries:
        fixed = sorted(int(x) for x in args.initial_boundaries.split(",") if x)
    prev_waf = float('inf')
    if fixed:
        # Start round above the highest fixed boundary
        top = fixed[-1]
        low = top * int(args.factor)
        high = top * int(args.factor ** args.parallel)
    else:
        low, high = args.init_low, args.init_high

    for round_idx in range(args.start_round, args.max_rounds + 1):
        best_c, best_waf, _ = round_n(round_idx, list(fixed), low, high,
                                       parallel=args.parallel, factor=args.factor)
        if best_c is None or best_waf is None:
            print("no valid result, stopping")
            break
        improvement = (prev_waf - best_waf) / prev_waf if prev_waf != float('inf') else 1.0
        print(f"round {round_idx}: best_waf={best_waf:.3f} "
              f"(prev {prev_waf:.3f}, improvement {improvement*100:.1f}%)")
        fixed = sorted(fixed + [best_c])
        if round_idx >= 2 and improvement < args.min_improvement:
            print("WAF reduction < threshold, stopping")
            break
        prev_waf = best_waf

        # Next round: place new boundary above current best_c
        low = best_c * int(args.factor)
        high = best_c * int(args.factor ** args.parallel)

    print(f"\nFINAL boundaries: {fixed} (+ inf)")


if __name__ == "__main__":
    main()
