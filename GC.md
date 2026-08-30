# GhostDelta_GC 의 G(u+θ) 정의 — GC vs GS

## 결정 룰

`periodic_ghost_delta_gc` (그리고 GS) 의 hill-climb 결정:

```
r · (eviction_rate − eviction_rate_in_ghost)  >  comp_rate_in_ghost − comp_rate
       └─────── Δflush ───────┘                  └────── Δcomp = G(u+θ)−G(u) ──────┘
→ raise target_valid_blk_rate by util_step
else → lower
```

양 변 모두 **rate per host page**. eviction_ratio_in_ghost 는 θ-larger ghost cache 의 evictCount 에서 derive — 즉 "ghost regime 의 flush rate".

대칭적으로 comp_rate_in_ghost 는 **u+θ regime 의 compaction rate** 이어야 함. 이게 G(u+θ).

## G(u+θ) 의 모델링: 세 가지 옵션

θ = util_step parameter. m = "ghost regime 에서 추가로 cleaned 되었어야 할 segment 수" 의 proxy. CB-sorted compactor queue 에서 boundary 결정.

### m 의 정의

코드 (`evict_policy_cost_benefit.cpp:78` `get_kth_segment_valid_cnt_for_free_segments`):

```
m = min { k : Σ_{i<k} (1 - u_i) ≥ θ · N }    (segment-equivalent free space 누적)
```

새 API (`get_ghost_sum_for_free_segments`):

```
m, Σv_i, Σ(seg-v_i) ← scan until Σ(seg-v_i)/seg ≥ θ · N
```

수식적으로는 둘 다 "0..(m-1) cleaning 으로 회수되는 free space ≥ θ·N·seg" 인 최소 m. **distribution 가정 없이 cumulative invalid 로 m 잡는 게 키 포인트** (segment 수에 θ 곱하는 단순 형식 m=θN 보다 한 단계 엄밀).

### 옵션 B (boundary) — 현재 `periodic_ghost_delta_gc` (GC)

```cpp
// log_cache.cpp ~796
valid_pages = compactor->get_kth_segment_valid_cnt_for_free_segments(θ·N);
u_m = valid_pages / seg;
scale = (1 - u_cur) / (1 - u_m);
ghost_compacted_blocks += valid_pages × scale;   // per real comp
```

의미: u+θ regime 의 **다음 한 번** GC cost = m번째 segment 의 valid_cnt.
Rate: **u_m / (1-u_m)** (steady-state 환산).

특성:
- **G(u+θ) 정의에 가장 직접적** (marginal/boundary).
- 단일 segment 라 **noisy** (m번째 의 age/u 가 튈 수 있음).

### 옵션 A (cumulative) — 새 `periodic_ghost_delta_gc_sum` (GS)

```cpp
// log_cache.cpp periodic_ghost_delta_gc_sum
auto s = compactor->get_ghost_sum_for_free_segments(θ·N);
rate = s.cum_valid / s.cum_invalid;              // = u_avg / (1-u_avg)
ghost_compacted_blocks_sum_ += dt × rate;
```

의미: 0..(m-1) cleaning **전체** 의 평균 cost rate.
Rate: **u_avg / (1-u_avg)** where u_avg = Σv_i / (m·seg).

특성:
- **Smoother** (m 개 평균).
- u_avg < u_m 이라 ghost rate 를 **항상 낮게 추정** (편향).
- Classical Greedy WAF 공식 `u/(1-u)` 와 dimensional consistency.

### 옵션 C (boundary window) — 미구현

```
G(u+θ) ≈ (1/(2k+1)) Σ_{i=m-k}^{m+k} v_i
```

A 의 smoothing + B 의 boundary 정확성. 구현 안 했음. 차이 크면 추가.

## per-epoch delta 와의 동치

한 θ-epoch (= θ·N·seg host writes 흐르는 동안):
- Ghost 가 한 일 = Σv_i (= cum_valid)
- Real 이 한 일 = compaction_blocks_delta_in_epoch
- Δcomp_in_epoch = Σv_i − real_delta

Epoch 길이 (= θ·N·seg) 로 나누면:
- Δrate = u_avg/(1-u_avg) − u_cur/(1-u_cur)

→ **per-epoch delta** 와 **per-host-page rate diff** 는 dimension 만 다른 같은 양.
→ GS 가 하는 rate 비교 = epoch 내 delta 비교와 동치.

## Workload / 재현 환경

- **Trace**: `/home/sejun000/alibaba_dwpd1to2_4x.trace` (Alibaba block trace, 4배 확장)
- **Total NAND pages**: 3,918,457,031 (4 KB 단위) ≈ 14.6 TiB host writes
- **Cache size**: 1,883,510,931,456 bytes (≈ 1.88 TB) — `(DEVICE_SIZE/8 align)` where DEVICE_SIZE=15 TB
- **Cold capacity**: 16,050,000,000,000 bytes (≈ 16.05 TB)
- **LBA scale**: 2
- **Block size**: 4096
- **rw_policy**: write-only, **trace_format**: csv, **prefill**: disabled

### Sweep script

`run_ma_sweep_fix.sh <POLICY> <UTIL_STEP> <MA_TYPE> <MA_WINDOW>` — r ∈ {2,4,6,8,10} 5-way 병렬.

GS sweep 재현:
```bash
./run_ma_sweep_fix.sh LOG_GREEDY_COST_BENEFIT_10_GS 0.02 ewma 3145728
```
- `3145728` = `DEFAULT_HALF_LIFE_IN_BLOCKS × 0.5` (default EWMA 반감기 절반)
- 다른 옵션: `0` (default fallback), `6291456` (default 동일), `12582912` (2×)

GC fix sweep (head-to-head 비교용):
```bash
./run_ma_sweep_fix.sh LOG_GREEDY_COST_BENEFIT_10_GC 0.02 ewma 3145728
```

각 sweep 5 runs × ~30분 wall (병렬이라 wallclock 도 ~30분).

### 단발 재현 (1개 r 만)

```bash
./cache_sim /home/sejun000/alibaba_dwpd1to2_4x.trace 1883510931456 \
    --rw_policy write-only --trace_format csv \
    --cache_policy LOG_GREEDY_COST_BENEFIT_10_GS \
    --cache_trace /mnt/nvme2n2/GS_pr4.trace \
    --cold_trace  /mnt/nvme2n2/GS_pr4.cold.trace \
    --cold_capacity 16050000000000 \
    --waf_log_file GS_pr4.waf.log \
    --periodic_ratio 4 --util_step 0.02 \
    --moving_avg_type ewma --moving_avg_window 3145728 \
    --stat_log_file GS.stat_pr4 --scale 2
```

## Wiring

- enum: `PeriodicMode::GhostDelta_GC_SUM` (`log_cache.h:41`)
- method: `LogCache::periodic_ghost_delta_gc_sum()` (`log_cache.cpp:251`)
- compactor API: `EvictPolicy::get_ghost_sum_for_free_segments(target_free_segs)` virtual, `CbEvictPolicy` 가 override
- cache_type: `LOG_GREEDY_COST_BENEFIT_10_GS` (`icache.cpp:464`)
- CLI: 기존 `--cache_policy LOG_GREEDY_COST_BENEFIT_10_GS --util_step 0.02 ...` 그대로

## 알려진 한계 (양쪽 다)

1. **Queue snapshot 가정**: m번째까지 정렬 상태를 "지금 이 순간" snapshot 에서 뽑음. 실제로 0..(m-1) cleaning 이 진행되면 그동안 새 segment 들 들어오고 age 진행됨 → 진짜 m번째와 다를 수 있음. 1-step lookahead 휴리스틱.
2. **CB-sort vs u-sort**: m번째 boundary 가 utilization 정확한 m-percentile 아님. CB score = (1-u)/u × age 라 age 가 섞임. 보통 약하게만 영향.
3. **mass-conservation 가정**: "θ·N·seg pages 가 free 되는 동안 정확히 θ·N·seg pages 의 host write 가 흡수" 가정. Steady-state 에서만 정확.

## 검증 항목 (수식 sanity)

논문 / reviewer 통과를 위해 empirical 확인이 필요한 것:
1. **Sign consistency**: target_valid_blk_rate 를 manual sweep 했을 때, 각 점에서 GS/GC 의 Δcomp 부호가 실제 ∂f/∂target 부호와 일치하는지.
2. **GC vs GS 의 trajectory 차이**: 같은 trace 에서 어느 쪽이 f 더 낮은지. 차이 크면 옵션 C 구현 가치.
3. **u_avg vs u_m 분포 격차**: m번째 와 0..(m-1) 평균의 격차가 클수록 A 의 편향이 큼.
