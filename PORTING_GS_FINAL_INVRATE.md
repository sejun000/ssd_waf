# PORTING — GS_FINAL invrate rule (D1 개선판)

`periodic_ghost_delta_gc_sum_final` 의 decision rule 을 **전역 λ 기반에서 "old-cohort
invalidation rate(invrate_sum)" 기반으로 교체**한 것 + 그걸 받치는 per-segment
invalidation-rate 머신을 다른 트리/baseline 으로 옮길 때 보는 문서.

> 코드가 source of truth. 라인넘버는 메모, 의심되면 grep. 정책 문자열:
> `LOG_GREEDY_COST_BENEFIT_10_GS_FINAL` → `PeriodicMode::GhostDelta_GC_SUM_Final`
> → `LogCache::periodic_ghost_delta_gc_sum_final()`.

---

## 0. TL;DR — rule 변화

| | LHS (GC 복사비용) | RHS (flush 대안비용) | RAISE 조건 | anti-stuck cap |
|---|---|---|---|---|
| **직전 D1 (lambda tag)** | `Gud` | `lambda · r` (전역 device invalidation rate) | `Gud < lambda·r` | 15 RAISE→1 강제 flush |
| **신규 (invrate)** | `Gud` | `invrate_sum · r` (WT≤v old cohort death rate) | `Gud < invrate_sum·r` | **제거** |

- `r = periodic_ratio_` (CLI `--periodic_ratio`, cold-tier WAF 가중).
- `Gud = compaction_ratio_in_ghost_cache.value()` = host write 1페이지당 GC 복사 valid page.
- **개선 핵심**: 전역 λ(모든 seg) 대신 **GC 가 실제로 청소할 old 영역(WT≤v)** 의 death rate
  만 봄. 단위는 동일(host page당 invalidation=page death), invrate_sum 은 λ 의 부분합.
- 양변 단위 일치(둘 다 host-write page당 page, 무차원), invrate_sum ≤ λ 항상.

---

## 1. 필수 변경 (rule 동작에 반드시 필요)

### 1-1. `segment.h` — per-segment invalidation rate (값타입, heap 없음)
`#include "emwa.h"` 추가 후, Segment base 에 멤버+메서드:
```cpp
static constexpr double INVAL_RATE_ALPHA = 0.1;     // 고정-α (튜닝 노브)
uint64_t invalidate_count_      = 0;   // 누적 host-invalidation (event마다 ++)
uint64_t invalidate_prev_count_ = 0;   // 직전 fold 시점 count
uint64_t invalidate_prev_ts_    = 0;   // 직전 fold 시점 host-time (0=미seed)
Ewma     invalidate_ewma_{INVAL_RATE_ALPHA};        // emwa.h 공유 커널

double invalidate_rate() const {                    // NaN-guard → 0
    return invalidate_ewma_.has_value() ? invalidate_ewma_.value() : 0.0;
}
void note_invalidation() { ++invalidate_count_; }   // per-event hot path = ++만
void fold_invalidate_rate(uint64_t now) {           // periodic tick에 미분→EWMA
    if (invalidate_prev_ts_ == 0) { invalidate_prev_ts_=now; invalidate_prev_count_=invalidate_count_; return; }
    if (now <= invalidate_prev_ts_) return;
    const uint64_t dH = now - invalidate_prev_ts_;
    const uint64_t dU = invalidate_count_ - invalidate_prev_count_;
    invalidate_prev_ts_=now; invalidate_prev_count_=invalidate_count_;
    invalidate_ewma_.update(static_cast<double>(dU)/static_cast<double>(dH));
}
void reset_invalidate_rate() {
    invalidate_ewma_.reset();
    invalidate_count_ = invalidate_prev_count_ = invalidate_prev_ts_ = 0;
}
```
> Segment base 에 둠(LogCacheSegment 만 아님): `sum_…` 이 evictor 의 `Segment*` 를 읽기 때문.
> 값타입이라 FTL `Block`(Segment 상속)에 unique_ptr 안 박힘 = 메모리 안전.

### 1-2. `log_cache_segment.h` — 재사용 시 초기화
`LogCacheSegment::reset()` 끝에 `reset_invalidate_rate();` 추가.

### 1-3. `log_cache.cpp::invalidate()` — per-event 카운팅
resident block 무효화 지점, `invalidate_blocks += 1` 과 **같은 블록**에:
```cpp
invalidate_blocks += 1;
loc.seg->blocks[loc.idx].valid = false;
--loc.seg->valid_cnt;
loc.seg->note_invalidation();        // ← 추가 (전역 카운터와 동일 이벤트 = 정확한 분할)
```

### 1-4. `evict_policy.h` — victim v 조회 + 순회 인터페이스
`#include <functional>` 후 EvictPolicy 에:
```cpp
struct VictimWtSpanResult {
    uint64_t max_wt = 0;            // "victim v" = cleaned set 중 max create_timestamp
    double   m      = 0.0;
    Segment* v_seg  = nullptr;
};
virtual VictimWtSpanResult get_victim_wt_span_for_free_segments(double target_free_segments) const { return {}; }
virtual void for_each_victim_in_order(const std::function<bool(Segment*)>& fn) const {}
```

### 1-5. `evict_policy_cost_benefit.{h,cpp}` — 위 두 개 override
```cpp
// victim v = get_ghost_sum_for_free_segments 와 같은 free-sum 누적 순회, max WT 추적
EvictPolicy::VictimWtSpanResult
CbEvictPolicy::get_victim_wt_span_for_free_segments(double target_free_segments) const {
    VictimWtSpanResult r;
    if (target_free_segments <= 0.0 || heap_.empty()) return r;
    double free_sum = 0.0;
    for (auto it = heap_.ordered_begin(); it != heap_.ordered_end(); ++it) {
        Segment* s = it->seg;
        const uint64_t v = s->valid_cnt;
        const uint64_t inv = (pages_in_segment > v) ? (pages_in_segment - v) : 0;
        const double inv_frac = (double)inv / (double)pages_in_segment;
        const uint64_t wt = s->get_create_time();
        if (r.v_seg == nullptr || wt > r.max_wt) { r.max_wt = wt; r.v_seg = s; }
        r.m += 1.0;
        if (free_sum + inv_frac >= target_free_segments) break;
        free_sum += inv_frac;
    }
    return r;
}
void CbEvictPolicy::for_each_victim_in_order(const std::function<bool(Segment*)>& fn) const {
    for (auto it = heap_.ordered_begin(); it != heap_.ordered_end(); ++it)
        if (!fn(it->seg)) break;
}
```

### 1-6. `log_cache.{h,cpp}` — WT≤v 합산
```cpp
// 선언(log_cache.h): double sum_invalidate_rate_in_wt_range(uint64_t wt_hi) const;
double LogCache::sum_invalidate_rate_in_wt_range(uint64_t wt_hi) const {
    double sum = 0.0;
    if (!evictor) return sum;
    // evictor(score_age_evict = -create_timestamp) = WT 오름차순 → wt_hi 넘으면 early-stop
    evictor->for_each_victim_in_order([&](Segment* s) -> bool {
        if (s->get_create_time() > wt_hi) return false;
        sum += s->invalidate_rate();
        return true;
    });
    return sum;
}
```

### 1-7. `log_cache.cpp::periodic_ghost_delta_gc_sum_final()` — fold loop + rule

**(a) fold loop** — 다른 ratio 갱신과 같은 `if (log_cache_timestamp % segment_size_blocks == 0)` 블록 안:
```cpp
if (evictor) {
    evictor->for_each_victim_in_order([this](Segment* s) -> bool {
        s->fold_invalidate_rate(log_cache_timestamp);
        return true;
    });
}
```

**(b) rule** — 기존 lambda(또는 marginal/Fpred) rule 을 교체:
```cpp
const double vic_target_free = util_step_ * (double)total_segments;   // = Final-form target
EvictPolicy::VictimWtSpanResult vspan;
double invrate_sum = 0.0;
if (compactor) {
    vspan = compactor->get_victim_wt_span_for_free_segments(vic_target_free);
    invrate_sum = sum_invalidate_rate_in_wt_range(vspan.max_wt);       // WT ≤ v 전부
}
const double lhs   = Gud;                            // GC 복사비용 / host page
const double rhs   = invrate_sum * periodic_ratio_;  // invrate·r / host page
bool         raise = (lhs < rhs);
// ↓ anti-stuck cap(15 RAISE→1 flush) 블록은 삭제. raise 는 위 비교가 전부.
```

### 1-8. anti-stuck cap 제거
- `log_cache.cpp`: 위 rule 직후의 `if (consec_raise_ >= 15) { raise=false; … }` 블록 삭제.
- `log_cache.h`: `int consec_raise_ = 0;` 멤버 삭제(다른 참조 없음).

### 1-9. `emwa.h` / `emwa.cpp` — `Ewma::update`/`updateWithAlpha` 헤더 inline
hot-path(현재는 fold loop) 호출 오버헤드 제거용. 두 정의를 .cpp→.h 로 이동(inline).
exp/log 쓰는 `updateWithUnits`·팩토리는 .cpp 그대로. *(per-event 가 ++count 라 필수는
아님; 안 옮겨도 동작. 옮기면 fold loop 가 inline 됨.)*

---

## 2. 진단용(OPTIONAL) — rule 과 무관, 포팅 안 해도 됨
GS_DECISION_LOG 의 `gcsim_*` 컬럼(what-if GC net-free 시뮬)용. 결정 로직 미사용.
- `istream.h` / `multi_hot_cold.{h,cpp}`: `ClassifyReadOnly()` (상태 불변 classify).
- `log_cache.{h,cpp}`: `GcNetFreeSim` 구조체 + `simulate_gc_net_free()`.
- `g_gs_dec_fp` 블록의 `gcsim_*` 7컬럼.
> rule 만 옮길 거면 이 절 전부 생략. 단 GS_DECISION_LOG 의 `vic_v_wt`/`invrate_sum`
> 2컬럼은 검증에 유용하니 넣는 걸 권장(§4).

---

## 3. 빌드 주의 (CLAUDE.md §8)
`segment.h`·`emwa.h`·`evict_policy.h` 는 널리 include 되는 ABI 헤더 → vtable/layout 변경.
stale `.o` 면 SIGSEGV 전례 있음. **반드시:**
```
make clean && make cache_sim
```
실행 중인 sim 이 있으면 link 시 ETXTBSY 날 수 있음 → `mv cache_sim cache_sim.bak` 후 make
(실행 중 프로세스는 inode 유지하므로 안전).

---

## 4. 실행 & 검증
직전 D1 baseline 과 동일 조건(D=1, hl=1seg, scale 2), rule 만 차이:
```bash
GS_DECISION_LOG="<tag>.gsdec.log" ./cache_sim /home/sejun000/alibaba_dwpd1to2_4x.trace 1883510931456 \
  --rw_policy write-only --trace_format csv \
  --cache_policy LOG_GREEDY_COST_BENEFIT_10_GS_FINAL \
  --cold_capacity 16050000000000 --waf_log_file <tag>.waf.log \
  --periodic_ratio 2.88 --util_step 0.02 \
  --moving_avg_type ewma --moving_avg_window 1572864 \
  --gs_decision_period_segs 1 --stat_log_file <tag>.stat --scale 2
```
(스크립트: `run_gsfinal_invrate.sh` 가 r=2.88/8.64 둘 다 런칭. cache_trace/cold_trace 덤프는 생략.)

**gsdec 새 컬럼**: `vic_v_wt invrate_sum` (헤더 끝). 검증 체크:
| 체크 | 기대 | awk |
|---|---|---|
| RHS = invrate_sum·r | 정확 일치 | `$10 ≈ $42*$3` |
| invrate_sum ≤ lambda | 항상 (부분합) | `$42 < $33` (비율 0.1~0.5) |
| NaN/음수 없음 | 0 | `$42==$42 && $42>=0` |
| v 따라 스케일 | v_wt↑→invrate_sum↑ | `$41` vs `$42` 동조 |
- WAF: `awk 'END{print $4/$3}' <tag>.waf.log` (Cold = col4/col3).

---

## 5. 설계 결정 (왜 이렇게)
- **독립 per-segment EWMA** (Σcount 후 한번에 미분 ✗): cohort `[..,v]` 멤버십이 tick마다
  churn(v 드리프트 + cleaned seg reset→count 0) → Σcount 비단조 → updateFromCumulative 가
  음수 dUc 접어 오염. 독립 EWMA 는 각 seg 가 자기 rate 를 smoothing, read 때만 cohort 선택
  → 면역. (EWMA 선형성상 멤버십 고정일 때만 둘이 동치.) → memory `invrate-independent-ewma`.
- **per-event = ++count, 미분은 periodic fold**: 다른 ratio 들과 동일 패턴(updateFromCumulative).
  1/dt EWMA(이전 시도)의 harmonic bias 없음, hot-path 가 단순 증가.
- **fold = `update(x)` 고정-α** (updateWithBlocks half-life 아님): fold cadence 가
  `segment_size_blocks` 로 일정 → dH≈const → 고정-α 와 등가. half-life/bias-corrected 가
  필요하면 per-seg MovingAverageRatio 로 교체(단 unique_ptr 이 FTL Block 까지 박힘).
- **anti-stuck cap 제거**: 결정을 순수 경제비교(`Gud < invrate_sum·r`)로. 강제 flush 없음.

---

## 6. 결과 / 상태 / TODO
**TEC (= host + comp + r·evict, TB) — alibaba_dwpd1to2_4x, gsD1, scale2, hl=1seg:**

| r | invrate (신규) | lambda (기존 D1) | Δ vs D1 | orig Fpred-Fghost(ref) |
|---|---|---|---|---|
| 2.88 | **32.35** (comp0.85 evict5.59 BUtil0.59) | 33.45 (comp7.39 evict3.71 BUtil0.78) | **−1.11 (−3.3%)** | 32.25 (~동률, +0.1) |
| 8.64 | **54.60** (comp6.69 evict3.76 BUtil0.78) | 55.74 (comp12.54 evict3.22 BUtil0.82) | **−1.13 (−2.0%)** | 55.13 (승, −0.53) |

- **invrate가 두 r 모두 lambda D1 baseline −2~3% 우위.** orig Fpred 대비는 high-r 승 / low-r 동률.
- 메커니즘: rule `Gud<invrate_sum·r`라 r이 RHS 직접 스케일 → low-r 에선 flush로 떠넘기고
  (comp↓ BUtil↓), high-r 에선 GC 유지(비싼 flush 회피). lambda 는 r 무관 과도 compaction.
- ⚠️ 이 결과는 **anti-stuck cap 있던 구 바이너리** (cap 제거 전 런칭). lambda baseline 도
  cap 포함이라 **rule 비교로는 공정**. cap-free 판은 빌드 완료(§1-8)지만 **미실행** → 재실행 시 갱신.
- 후속 아이디어: GC victim score 에 `invalidate_rate` 직접 반영("약간 hot" 우대 = sweet-spot;
  너무 cold→재복사 tax, 너무 hot→낭비). age 프록시(score_warm_first) 대비 이기는지 검증 필요.

관련 memory: `invrate-independent-ewma`, `fpred_fghost_decision`, `gs-final-util-step-override`, `marginal-rule-loses`.
