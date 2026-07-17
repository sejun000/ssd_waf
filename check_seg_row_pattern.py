#!/usr/bin/env python3
"""Per-segs check: do compaction/eviction only advance every `segs` rows?
For each seg, look at 1-row deltas of compacted_blocks and evicted_blocks
and report:
  - fraction of rows with dcomp=0
  - fraction of rows with devict=0
  - 0-streak length distribution for dcomp (avg, p50, p90, max)
  - 0-streak length distribution for devict
"""
import os, re, sys
import numpy as np

KEY_RE = re.compile(r"(\w+):?\s+(-?\d+(?:\.\d+)?)")
PERIODS = [1, 2, 4, 8, 16, 32, 64, 128]

def find_stat(p):
    for rtag, r in [(864, 8.64), (8, 8.0)]:
        path = f"LOG_GREEDY_COST_BENEFIT_10_GS_us02_ewma_hl1572864_gsv4_segs{p}.stat_pr{rtag}"
        if os.path.exists(path) and os.path.getsize(path) > 0: return path, r
    return None, None

def parse(path):
    rows = []
    with open(path) as fh:
        for line in fh:
            if not line.startswith("LOG_GREEDY"): continue
            kv = dict(KEY_RE.findall(line))
            try:
                rows.append((int(kv["compacted_blocks"]),
                             int(kv["evicted_blocks"])))
            except (KeyError, ValueError): continue
    return np.array(rows, dtype=np.int64)

def streak_lengths(zeros_bool):
    # length of consecutive True runs in a boolean array
    lens = []
    cur = 0
    for v in zeros_bool:
        if v: cur += 1
        else:
            if cur > 0: lens.append(cur)
            cur = 0
    if cur > 0: lens.append(cur)
    return np.array(lens) if lens else np.array([0])

print(f"{'seg':>4}  {'rows':>5}  |  {'comp_zero%':>10}  {'evict_zero%':>11}  |  "
      f"{'comp_streak_avg':>16} {'p50':>4} {'p90':>5} {'max':>5}  |  "
      f"{'evict_streak_avg':>17} {'p50':>4} {'p90':>5} {'max':>5}")
print("-"*125)
for p in PERIODS:
    path, _ = find_stat(p)
    if path is None: continue
    a = parse(path)
    if len(a) < 3: continue
    comp = a[:,0]; evict = a[:,1]
    dcomp  = comp[1:]  - comp[:-1]
    devict = evict[1:] - evict[:-1]
    n = len(dcomp)
    cz = (dcomp == 0).sum()
    ez = (devict == 0).sum()
    cs = streak_lengths(dcomp == 0)
    es = streak_lengths(devict == 0)
    print(f"{p:>4}  {n:>5}  |  {100*cz/n:>9.1f}%  {100*ez/n:>10.1f}%  |  "
          f"{cs.mean():>16.2f} {int(np.median(cs)):>4} {int(np.percentile(cs,90)):>5} {int(cs.max()):>5}  |  "
          f"{es.mean():>17.2f} {int(np.median(es)):>4} {int(np.percentile(es,90)):>5} {int(es.max()):>5}")
