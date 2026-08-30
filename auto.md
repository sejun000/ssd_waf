# GhostDelta_GC_AUTO — GP/UCB autotuner

GhostDelta_GC 위에 얹은 GP/UCB autotuner. 4D arm 공간에서 (dir, half-life, compare-window, step) 을 round 단위로 탐색. r=4 single-run 결과: 결국 **stock GC 대비 10% 나쁨** — 신호 자체보다 **tuner 가 못 학습** 한 게 원인.

## Workload / 재현 환경

- **Trace**: `/home/sejun000/alibaba_dwpd1to2_4x.trace` (Alibaba block trace, DWPD 1~2 대역, 4배 길이 확장본)
- **Trace format**: csv
- **Total NAND pages**: 3,918,457,031 (4K 페이지) — 약 14.6 TiB worth of host writes
- **LBA scale**: 2 (워크로드 LBA × 2)
- **Cache size**: 1,883,510,931,456 bytes (≈ 1.88 TB = DEVICE_SIZE/8 의 align)
  - `DEVICE_SIZE = 15_000_000_000_000` (15 TB)
  - `ALIGN = 13_079_937_024`
  - `CACHE = ((DEVICE_SIZE/8 + ALIGN-1) / ALIGN) * ALIGN`
- **Cold capacity**: 16,050,000,000,000 bytes (≈ 16.05 TB)
- **Block size**: 4096 (4 KB)
- **rw_policy**: write-only
- **Prefill**: disabled (cold tier 채워두지 않음 → 초기엔 cache hit 만 cold 로 evict 됨)

## AUTO 재현 명령

```bash
./cache_sim /home/sejun000/alibaba_dwpd1to2_4x.trace 1883510931456 \
    --rw_policy write-only \
    --trace_format csv \
    --cache_policy LOG_GREEDY_COST_BENEFIT_10_GC_AUTO \
    --cache_trace /mnt/nvme2n2/AUTO_pr4.trace \
    --cold_trace  /mnt/nvme2n2/AUTO_pr4.cold.trace \
    --cold_capacity 16050000000000 \
    --waf_log_file AUTO_pr4.waf.log \
    --periodic_ratio 4 \
    --util_step 0.02 \
    --moving_avg_type ewma \
    --moving_avg_window 6291456 \
    --stat_log_file AUTO.stat_pr4 \
    --scale 2
```

- `--moving_avg_window 6291456` = `DEFAULT_HALF_LIFE_IN_BLOCKS` (262144·6·4). 0 줘도 fallback.
- `--periodic_ratio` (r) 가 reward = -(r·flush_rate + comp_rate) 의 r.
- Autotune CSV 는 `--stat_log_file` 의 `.stat` → `.autotune` 치환 후 `.csv` 붙여서 생성 (`icache.cpp:483~509`).
- 풀 trace 1회 ≈ 30분 wall (Linux 6.18, 단일 코어 99%).

## Policy / Wiring

- enum: `PeriodicMode::GhostDelta_GC_AUTO` (`log_cache.h:40`)
- cache_type: `LOG_GREEDY_COST_BENEFIT_10_GC_AUTO` (`icache.cpp:483`)
- 동작: GhostDelta_GC 와 동일한 비교 룰, but `dir` / `util_step` / `compare_segs` / `EWMA half-life` 가 라운드마다 GP 가 고른 arm 으로 교체.
- Round close: 1 TiB host write 마다. close 시 reward 관측 → GP update → 다음 arm pick.
- Round 0: `DefaultArm()` 반환 — 첫 1 TiB 동안 stock GC 와 동일.

## Arm 공간 — 360개

```
dir              ∈ {-1, +1}                                                (2)
half_life_blk    ∈ {1572864, 3145728, 6291456, 12582912, 25165824, 50331648} (6)
window_segs      ∈ {1, 2, 4, 8, 16, 32}                                    (6)
step_pct         ∈ {0.005, 0.01, 0.02, 0.04, 0.08}                          (5)
```

총 2×6×6×5 = **360 arms**.

## Reward

Round 끝나면:
```
flush_rate = evicted_delta / host_writes_delta
comp_rate  = compacted_delta / host_writes_delta
f          = r·flush_rate + comp_rate
reward     = -f
```

GP 가 reward 최대화 = f 최소화 방향으로 탐색.

## GP / UCB 세부

- Cholesky-based GP, RBF/ARD kernel
- UCB: μ + β·σ where **β = 0.5** (was 2.0 — reduced to favor exploitation given heavy under-sampling)
- Observation noise modeling 으로 stability 확보

## r=4 single-run 결과 (14 TiB)

Cumulative reward (전체 trace 누적, host pages ≈ 3.76 B):

|                     | f       | comp B  | evict B |
|---------------------|---------|---------|---------|
| AUTO β=0.5 r=4      | **1.4354** | 0.675   | 1.180   |
| AUTO β=2.0 r=4      | 1.5866  | 1.340   | 1.160   |
| stock GC r=4 fix    | 1.4416  | 0.630   | 1.200   |

→ β=2.0 → β=0.5 로 낮춘 결과 **stock GC 와 사실상 동등** (par, −0.5% — counter-noise 안). prior β=2.0 는 comp 가 2.1× 많아 손해였고, β=0.5 는 comp 0.675 B 로 stock(0.63 B) 거의 매칭.

**주의**: per-round f 는 round 14 에서 2.01 — 끝까지 stock 보다 나쁨. cumulative match 는 round 2~3 의 lucky low-f (warmup 단계, cache 아직 채워지는 중) 가 평균 끌어내린 것. 즉 GP 가 정착해서 stock 을 따라잡은 게 아님.

## Autotune CSV 로깅

`LOG_GREEDY_COST_BENEFIT_10_GC_AUTO_*.autotune_pr*.csv` 매 round 1줄:

```
round, host_TiB, closed_dir, closed_hl, closed_win, closed_step,
flush_rate, comp_rate, f, reward, n_train,
next_dir, next_hl, next_win, next_step, next_mu, next_sigma
```

- "closed_*" = 이번 round 에 적용되어 reward 를 만든 arm
- "next_*" = round close 후 GP 가 뽑은 다음 arm
- mu/sigma = next arm 에 대한 GP posterior

### 관측된 trajectory (r=4, 14 rounds)

| round | β=2.0 closed arm | f | σ_next | β=0.5 closed arm | f | σ_next |
|---|---|---|---|---|---|---|
| 1 | default (1,6.3M,8,0.02) | 0.000 (warmup) | 1.000 | default (1,6.3M,8,0.02) | 0.000 (warmup) | 1.000 |
| 5 | (1,1.5M,16,0.005) | 1.724 | 0.926 | (1,3.1M,16,0.02) | 1.777 | 0.581 |
| 8 | (1,6.3M,8,0.08) | 1.939 | 0.875 | (1,3.1M,4,0.005) | 1.768 | **0.139** |
| 14 | (-1,12.5M,1,0.08) | 3.346 | 0.519 | (1,50.3M,8,0.005) | 2.013 | 0.733 |

- β=2.0: σ 천천히 감소(0.5), 마지막 round 에 worst-arm 시도 → f=3.3 으로 튐.
- β=0.5: σ r8 에 0.14 로 급락 (한 pocket 에 confident), but 그 arm 의 reward 가 기대 미달 → r14 σ 0.73 로 다시 반등. f 는 1.5~2.0 사이 stable, never blows up.
- β=0.5 는 explore 안 함 — dir=−1 을 round 2 한 번만 시도 후 +1 로 고정. 후반 arms 모두 step≤0.04 / win∈{4,8} 같은 좁은 영역 안 머무름.

## 진단: explore-heavy 실패 (β=2.0)

- arm 360 개 vs 관측 14 개 → 35× under-sampled.
- β=2.0 UCB 가 high-σ region 으로 자꾸 끌고 감 → exploitation 안 됨.
- Round size 1 TiB = ~10 분 wall — observation rate 가 너무 느림.

## β=0.5 후속 진단

- cumulative f 가 stock GC 와 par 까지 회복했으나, 이는 **warmup 운빨** (round 2 f=0.10, round 3 f=1.3 가 평균 끌어내림). round 5+ 부터는 stock 보다 일관되게 나쁨 (1.5~2.0).
- GP 가 한 arm pocket 에 빠르게 정착(σ→0.14)했지만, 그 pocket 도 stock 만큼 좋지 않음 → **arm 공간 자체에 stock-equivalent 가 없거나, reward signal 이 약함**.
- explore 부족도 별개 문제: dir=−1 한 번도 재시도 안 함 → 절반 공간(180 arms) 미탐색.
- 즉 β 만 만져서는 한계 — 다음은 arm grid 축소 + round 축소가 자연스러움. 혹은 신호 자체(GC.md G(u+θ)) 의심 라인 (§ "신호 자체에 대한 의심").

## 다음 step 후보

1. **β reduction** (2.0 → 0.5): exploit 비중 높임. Side-effect: stuck in local optimum 위험.
2. **Arm grid 축소** (360 → 6~12): 예를 들어 (dir, hl) 만 grid, window/step 고정.
3. **Round size 축소** (1 TiB → 256 GiB): obs 4× 늘림. EWMA reset latency 확인 필요.

조합도 가능. 가장 efficient 한 건 **arm 축소 + round 축소** 동시. β 는 둘 다 한 뒤 영향 보고 조정.

## 신호 자체에 대한 의심

AUTO 가 망한 게 tuner 문제인 것 같았는데, GC.md 의 G(u+θ) formulation 자체에 약점이 있다면 GP 가 의미 없는 reward landscape 학습 중일 수 있음. 그 가설 검증을 위해 **GS** (cumulative formulation) 가 stock GC 보다 잘 작동하는지부터 확인 → 잘 작동하면 신호 OK, 그 다음 AUTO 재시작.

## Wiring 위치

- `log_cache.cpp::periodic_ghost_delta_gc_auto()` (`log_cache.cpp:278`):
  - 매 periodic tick: `gp_tuner_->observe(host_writes, evicted, compacted)`
  - Round close 감지 시: `close_round_and_select()` 로 arm 교체 + CSV 로깅
  - half-life 바뀐 경우만 `setMovingAverage()` 호출 (EWMA churn 회피)
- `auto_tune/gp_tuner.{h,cpp}`: GP 본체, arm 정의, UCB 로직, telemetry accessor
- CSV 경로 derivation: `foo.stat_pr4` → `foo.autotune_pr4.csv`

## 알려진 한계

1. **EWMA churn on arm switch**: half-life 바뀌면 ratio 객체 7개 재생성 → 직후 몇 tick 동안 ratio 가 noisy. 현재는 half-life 안 바뀌면 skip 으로 완화.
2. **Reward bias from warmup rounds**: round 1~5 는 cache 가 아직 채워지는 중이라 flush/comp rate 가 비대표적. Round 0 만 default arm 으로 두고 1+ 부터 GP 가 reward 받음 — 더 긴 warmup 필요할 수 있음.
3. **state 의존성**: GhostDelta_GC 의 hill-climb state (target_valid_blk_rate) 가 arm 바뀌어도 지속됨 → 이전 arm 의 결정이 다음 arm 의 reward 에 영향. arm 간 독립성 약함.
