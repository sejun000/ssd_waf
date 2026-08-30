# REFLASH 실행 파라미터 (다른 trace 적용용)

REFLASH = GS_FINAL 정책의 D=1 setting. PrGh decision: `LHS = r·waf·(F_pred − F_ghost)`, `RHS = Gud`, raise iff LHS > RHS.

## 핵심 파라미터 (고정)

| 항목 | 값 | 비고 |
|---|---|---|
| `--cache_policy` | `LOG_GREEDY_COST_BENEFIT_10_GS_FINAL` | |
| `--gs_decision_period_segs` | `1` | **REFLASH = D=1** |
| `--util_step` | `0.02` | setPeriodicMode가 D 기반으로 auto-derive (`util_step_ = seg_blocks·D / total_blocks`) — CLI 값은 무시될 수 있음 |
| `--moving_avg_type` | `ewma` | |
| `--moving_avg_window` | `1572864` | EWMA half-life (blocks) |
| `--scale` | `2` | |
| `--rw_policy` | `write-only` | |
| `--trace_format` | `csv` | trace 포맷에 맞춰 조정 |

## Trace-dependent 파라미터

```bash
TRACE=/path/to/your.trace                # 입력 trace
DEV=<device size bytes>                  # ex) 15000000000000 (15 TB)
COLD=<cold tier capacity bytes>          # ex) 16050000000000 (16.05 TB)
ALIGN=13079937024                        # segment_size_blocks * page_size align
CACHE=$(( ((DEV / 8 + ALIGN - 1) / ALIGN) * ALIGN ))   # cache = device/8 round-up
```

`DEV / 8` 의 분모 8 은 hot cache 비율(=12.5%). trace 특성에 맞춰 변경.

## Sweep 파라미터

- `--periodic_ratio R` : NAND WAF 비용가중치. 측정용으로 `R ∈ {2.88, 5.76, 8.64, 11.52, 14.40}` 또는 단일 값.
- 결과 stat: `${TAG}.stat_pr${RT}` (RT = R 에서 점 제거)

## 최소 실행 예시

```bash
./cache_sim "$TRACE" "$CACHE" \
    --rw_policy write-only --trace_format csv \
    --cache_policy LOG_GREEDY_COST_BENEFIT_10_GS_FINAL \
    --cache_trace /mnt/nvme2n2/reflash_r${RT}.trace \
    --cold_trace  /mnt/nvme2n2/reflash_r${RT}.cold.trace \
    --cold_capacity "$COLD" \
    --waf_log_file reflash_r${RT}.waf.log \
    --periodic_ratio "$R" --util_step 0.02 \
    --moving_avg_type ewma --moving_avg_window 1572864 \
    --gs_decision_period_segs 1 \
    --stat_log_file reflash_r${RT}.stat \
    --scale 2
```

## 참고 (현 baseline trace)

- alibaba_dwpd1to2_4x: host=14.00 TiB, DEV=15TB, COLD=16.05TB, ALIGN=13079937024
- 결과 (D=1):
  - r=2.88 → TEC=29.33, r=5.76 → 40.02, r=8.64 → 50.14, r=11.52 → 59.26, r=14.40 → 68.75
- 참조 launcher: `run_gs_PrGh_all.sh` (D-sweep), `run_gs_PrGh_rsweep.sh` (r-sweep at D=1)
