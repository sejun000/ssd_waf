# read_cache branch — NoDaP debugging context

Snapshot date: 2026-05-01.

## Goal

Implement NoDaP (Near-optimal Data Placement, FAST'26 DOGI paper §3.1)
in cache_sim as `LOG_NODAP_88`, then use the paper's hierarchical offline
grid search to find optimal BIR boundaries for our YCSB-A trace.

Paper-claim WAF (oracle): ~1.0. Our observed WAF: ~7.645 — far off,
debugging the gap.

## Repo state

Branch: `read_cache` → `origin/read_cache`.
Recent commits:
- `6435be2 Add gen_fio_zipf standalone trace generator`
- `12a617f Add NoDaP oracle placement and DOGI/log_cache extensions`
- `03208e0 Add DOGI stream telemetry and align GC stages`

Currently uncommitted (instrumentation in progress):
- `dogi/app/global.h`, `dogi/app/global.cc`: added
  `g_nodap_victim_case_valid_ratio_sum[3]` and `g_nodap_sealed_per_class[40]`
- `log_cache.cpp`: increment/decrement of those gauges, dump in
  `dogi_cmp.log` line as `nodap_case_avg_vr=...` and
  `nodap_sealed_per_class=...`

## Trace + cache config

- Trace: `/home/sejun000/workloads/logs/cache_io_fio_ycsb_70_20260401_033750.trace`
  (YCSB-A blkparse_csv, 20.7 GB)
- Cache size: 130 GB (`130_799_370_240`)
- Cold capacity: 1.07 TB (`1_070_000_000_000`)
- `--periodic_ratio 2.88`, `--rw_policy all`
- prefill OFF, lba_remap OFF (oracle requires no prefill — pre-pass would
  desync otherwise)

## NoDaP implementation

Files:
- `nodap_stream.{cpp,h}`: oracle pre-pass (per-LBA invalidation queue
  in synthetic_ts units = global block-write event index). Classify
  pops front of queue at host write, stashes in `current_inv_` for
  GC-time reuse. BIR = `inv_time - global_timestamp`.
- `icache.cpp` `LOG_NODAP_88`: parses `NODAP_BIR` env (e.g. "1000,inf"),
  loads oracle from input trace, wires NodapStream into LogCache with
  `score_nodap_expired` compactor and:
  - `setDogiGcMode(true, 0.12)` (DOGI-style GP threshold trigger)
  - `setDogiKeepSegTimestamp(false)` (GC target's create_timestamp
    pulled back to oldest copied block)
  - `setDogiShareActiveSegments(true)` (host + GC share active table)

Score formula `score_nodap_expired` (icache.cpp):
- valid >= 95% segment_blocks → -1e18 (case 0, fully-valid skip)
- idx < g_n_idx AND age > BIR_upper[idx] → 1e18 + age (case 1, expired)
- else → (idx - g_n_idx) * seg_blocks - valid_cnt (case 2, greedy)

`age = g_timestamp - create_timestamp`, where `g_timestamp` is updated
once per GC trigger in `check_and_evict_if_needed`. Heap update() runs
on every invalidate hitting a sealed seg → score is mostly fresh.

## Timestamp unit verification

- `synthetic_ts` (pre-pass) increments per block kv pair globally
  → BIR between two writes to same lba = total block-writes across ALL
  lbas in between.
- `log_cache_timestamp` runtime: only one increment site at
  `log_cache.cpp:420` inside the host insert path
  (`++log_cache_timestamp`). GC compaction at `evict_and_compaction`
  does NOT increment it. So units match: log_cache_timestamp is
  cumulative HOST block-writes.

This rules out the most obvious unit mismatch.

## Round 1 grid search results (N=2, BIR = [G_1, inf])

10 parallel runs with `nodap_grid_search.py` (factor 2x, [1K..512K]):

| G_1   | host_blk  | compacted_blk | gc_count | avg_vr | WAF    |
|-------|-----------|---------------|----------|--------|--------|
| 1K    | 338264064 | 2247819360    | 26014    | 0.8790 | 7.6452 |
| 2K    | 338264064 | 2248091025    | 26018    | 0.8790 | 7.6460 |
| 4K    | 338264064 | 2247850403    | 26015    | 0.8790 | 7.6453 |
| 8K    | 338264064 | 2247389830    | 26010    | 0.8790 | 7.6439 |
| 16K   | 338264064 | 2248086759    | 26018    | 0.8790 | 7.6460 |
| 32K   | 338264064 | 2248510224    | 26023    | 0.8790 | 7.6472 |
| 64K   | 338264064 | 2248335869    | 26021    | 0.8790 | 7.6467 |
| 128K  | 338264064 | 2248605014    | 26025    | 0.8789 | 7.6475 |
| 256K  | 338264064 | 2248362055    | 26022    | 0.8789 | 7.6468 |
| 512K  | 338264064 | 2248418424    | 26023    | 0.8789 | 7.6469 |

**WAF, gc_count, compacted_blocks, avg_vr all virtually identical** across
the 512x BIR range. Only the case classification labels shift.

## Case-x-class breakdown (round 1)

| G_1   | case0 (skip) | case1 (expired G_0) | case2 (greedy G_1) |
|-------|--------------|---------------------|--------------------|
| 1K    | 4571 (idx=1) | 1541 (idx=0)        | 19902 (idx=1)      |
| 4K    | 5112 (idx=1) | 1724 (idx=0)        | 19179 (idx=1)      |
| 16K   | 5917 (idx=1) | 1975 (idx=0)        | 18126 (idx=1)      |
| 64K   | 8311 (idx=1) | 2284 (idx=0)        | 15426 (idx=1)      |
| 128K  | 9462 (idx=1) | 2478 (idx=0)        | 14085 (idx=1)      |
| 256K  | 10592(idx=1) | 2779 (idx=0)        | 12651 (idx=1)+1@0  |
| 512K  | 11265(idx=1) | 3428 (idx=0)        | 11330 (idx=1)+3@0  |

case0 always at idx=1 (cold pool), case1 always at idx=0 (hot pool),
case2 nearly all at idx=1.

## Working theory

- Oracle is correct and timestamp units match.
- N=2 with a small G_0 fraction means hot-tier traffic is small
  (case1 count ≈ G_0 traffic share). Cold tier dominates.
- Cold-tier GC valid_ratio = 0.879 → asymptote 1/(1-0.879) = 8.27.
  Observed 7.645 ≈ f_hot · 1 + (1-f_hot) · 8.27 with f_hot ≈ 0.1.
- So WAF ≈ structurally bound by N=2.

But ONE anomaly: avg_vr stays exactly 0.879 across BIR settings even as
case mix shifts (case1: 1541→3428). If case1 victims were near-empty
(true oracle behavior for "expired hot" segments), doubling case1
share should drop avg_vr noticeably. It doesn't.

→ Suspect: case1 victims also have valid_ratio ~0.879. The "expired
hot" segments aren't actually empty when picked. Possible reasons:
1. With `keep_seg_timestamp=false`, GC-target segments inherit oldest
   copied block's create_timestamp → segment age is huge → case1
   condition (age > BIR) fires for nearly every G_0 segment regardless
   of actual valid state → score 1e18+age picks oldest, not emptiest.
2. Heap stale-score: if case1 score (1e18 + age) wins regardless of
   valid_cnt, picks oldest expired even at high valid.

## Currently running test

PID 666747 (started 2026-05-01 03:51).
Args: `NODAP_BIR=1000,inf` on the YCSB-A trace.
Output:
- stdout: `/tmp/nodap_test.stdout`
- waf log: `/tmp/nodap_test.waf.log`
- stat log: `/tmp/nodap_test.stat.log` (auto-renamed to
  `LOG_NODAP_88.stat.log.<ts>`)
- dogi_cmp: `LOG_NODAP_88.dogi_cmp.log.<ts>`

This run includes the new `nodap_case_avg_vr=v0/v1/v2` and
`nodap_sealed_per_class=...` fields. Pre-pass takes ~5-7 min, full run
~30 min. Wakeup scheduled 04:17 to analyze.

Expected output to inspect:
```
grep "nodap_case_avg_vr\|nodap_sealed_per_class" \
     LOG_NODAP_88.dogi_cmp.log.<latest> | tail -3
```

Hypothesis decision tree:
- case1_avg_vr ~0.88 ≈ case2_avg_vr → confirms theory 1/2 above.
  Fix: rethink score formula (e.g. tiebreak by valid_cnt within
  expired tier, or tighter age check), or set `keep_seg_timestamp=true`
  to avoid create_timestamp pull-back.
- case1_avg_vr << case2_avg_vr (e.g. 0.05 vs 0.95) → case1 reclaims
  ARE near-empty. Then WAF unchanged because hot-tier savings are
  numerically negligible vs cold-tier amplification at N=2. Fix:
  proceed to N=3+ via grid search.

## Key files for resume

- `/home/sejun000/ssd_waf/nodap_stream.{cpp,h}` — oracle
- `/home/sejun000/ssd_waf/icache.cpp` lines 87-113 — score_nodap_expired
- `/home/sejun000/ssd_waf/icache.cpp` lines 657-714 — LOG_NODAP_88 setup
- `/home/sejun000/ssd_waf/log_cache.cpp` lines 596-715 — GC trigger +
  victim case classification
- `/home/sejun000/ssd_waf/log_cache.cpp` lines 802-881 —
  evict_and_compaction (GC path)
- `/home/sejun000/ssd_waf/log_cache.cpp` lines 1183-1212 — dogi_cmp.log
  dump
- `/home/sejun000/ssd_waf/evict_policy_cost_benefit.cpp` —
  CbEvictPolicy::choose_segment validates top-K=10
- `/home/sejun000/ssd_waf/nodap_grid_search.py` — orchestrator for
  hierarchical N=2..K BIR search
- `/home/sejun000/ssd_waf/bir_dist.py` — per-group BIR distribution
  analysis from trace

## Useful commands

Single run with custom BIR:
```
NODAP_BIR=1000,inf ./cache_sim <trace> 130799370240 \
    --rw_policy all --trace_format blkparse_csv \
    --cache_policy LOG_NODAP_88 --cold_capacity 1070000000000 \
    --waf_log_file /tmp/x.waf.log --stat_log_file /tmp/x.stat.log \
    --periodic_ratio 2.88
```

Parse WAF from a stdout:
```
rename=$(grep "stat log renamed" <stdout> | tail -1 | awk '{print $NF}')
grep " compacted_blocks: " "$rename" | tail -1
```

Grid search resume from fixed boundaries:
```
python3 nodap_grid_search.py --initial_boundaries 1000 \
    --start_round 2 --max_rounds 6 --parallel 10 --factor 2.0
```

## Open questions to resolve next session

1. case1 vs case2 avg_vr from current running test → settle which
   theory is right.
2. If theory 1/2: try `setDogiKeepSegTimestamp(true)` and rerun;
   compare case1_avg_vr.
3. Independent check: distribution of `sealed_per_class` over time —
   is G_0 actually small (few sealed G_0 segments at any moment)?
4. If hot-tier optimization is structurally limited, proceed to
   round 2+ with confidence rather than chasing a phantom bug.
