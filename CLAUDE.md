# Working Notes — MovingAverage Refactor & Periodic Policy Fixes

세션 작업 요약. 코드 자체가 source of truth — 여기 적힌 라인넘버/파일 경로는 메모일 뿐, 변경 시 자동 갱신되지 않으니 의심되면 grep 으로 재확인할 것.

## 1. MovingAverage 추상화 (Simple Factory 패턴)

**의도**: ratio 기반 hill-climb 정책에서 EWMA/SMA 를 CLI 로 갈아끼울 수 있게.

- `emwa.h`: `MovingAverage` 추상 베이스 추가. 가상 인터페이스 `updateWithBlocks / value / has_value / reset` + virtual dtor.
- `emwa.h`: `Ewma : public MovingAverage` (override 키워드로 재정의)
- `sma.h` / `sma.cpp` (신규): `Sma : public MovingAverage`. 블록-가중 rolling SMA. 윈도우 초과 시 front partial-trim.
- `emwa_ratio.h`: `EwmaRatio` → `MovingAverageRatio` 로 이름 변경. 내부에 `std::unique_ptr<MovingAverage>`. move-only. `using EwmaRatio = MovingAverageRatio;` legacy alias 유지.
- Simple Factory: `MovingAverageRatio::Make(type, window_blocks)` — `type=="sma"` 면 Sma, 아니면 Ewma(half-life).

**Makefile**: `SRCS_cache_sim` 에 `sma.cpp` 추가됨.

## 2. LogCache::setMovingAverage 셋터

`log_cache.h` 에 멤버 추가:
- `std::string moving_avg_type_ = "ewma";`
- `double moving_avg_window_ = (double)DEFAULT_HALF_LIFE_IN_BLOCKS;`
- 셋터에서 7개 ratio 객체를 한꺼번에 `Make()` 로 교체:
  `compaction_ratio`, `eviction_ratio`, `eviction_ratio_in_ghost_cache`, `compaction_ratio_in_ghost_cache`, `ghost_util_ratio`, `net_free_seg_ratio_`, `gc_valid_pages_ratio_`.

**왜 ctor 가 아니라 셋터?** ctor 시그니처 변경은 사용자 거부. setter 패턴으로 합의.

## 3. CLI 배선

- `cache_sim.cpp`: `--moving_avg_type` / `--moving_avg_window` 파싱 → `createCache` 인자로 전달.
- `icache.cpp::createCache`: 시그니처에 `const std::string& moving_avg_type = "ewma", double moving_avg_window = 0.0` 추가. LogCache 변종(`LOG_GREEDY_COST_BENEFIT_10`, `_TDELTA`, `_GC`, `_GC_NAND`, `_GD002`) 마다 `lc->setMovingAverage(...)` 호출.

## 4. periodic_t_delta refactor

`log_cache.cpp::periodic_t_delta()`:
- `compaction_ratio` / `eviction_ratio` 를 `updateFromCumulative(timestamp, cumulative_count)` 로 갱신 (period = `segment_size_blocks/4`).
- hill-climb 비교 주기 = `segment_size_blocks * 8`.
- `f_curr = compaction_ratio.value() + periodic_ratio_ * eviction_ratio.value()`.
- 직전 f 보다 커지면 방향 반전.

## 5. current_waf 변수 충돌 버그 수정

**증상**: GC_NAND 의 `current_waf` 가 항상 ~0 → cold-tier WAF 가중치가 안 먹음.

**원인**: ICache 베이스(`icache.h:72-73`)의 `last_ftl_host_write_pages` / `last_ftl_nand_write_pages` 를 `icache.cpp:695-697` WAF 로깅이 매 호출 `=` 로 덮어씀. LogCache::periodic 이 같은 이름으로 delta 를 빼려 했지만 매번 클러버됨.

**조치**: LogCache 전용 `lc_last_ftl_host_pages` / `lc_last_ftl_nand_pages` 로 rename (사용 안 되던 `last_ftl_host_written` / `last_ftl_nand_written` 자리에 대체). 베이스 클래스 변수와 분리.

```cpp
// log_cache.cpp:199-216
uint64_t host_delta = ftl.GetHostWritePages() - lc_last_ftl_host_pages;
uint64_t nand_delta = ftl.GetNandWritePages() - lc_last_ftl_nand_pages;
current_waf = host_delta > 0 ? double(nand_delta)/host_delta : 0.0;
lc_last_ftl_host_pages += host_delta;
lc_last_ftl_nand_pages += nand_delta;
```

## 6. stat 파일 확장

`log_cache.cpp::print_stats` fprintf 에 `ftl_host_pages`, `ftl_nand_pages` 컬럼 추가 (cold-tier WAF 검증용).

## 7. 알려진 한계

- **Cold-tier WAF=1.0**: 현재 trace 로는 cold_capacity=16TB / prefill=80% / 누적 eviction ~5TB 라 FTL GC 트리거 안 됨 → GC_NAND 정책의 `current_waf` 자체가 의미가 없음. 사용자가 trace 새로 만들겠다고 함.

## 8. Static-analysis 체크리스트 (chain 끝나면 점검)

1. **Move semantics**: `MovingAverageRatio` 가 unique_ptr 보유 → copy 막혀 있는지, 멤버 7개 모두 setter 에서 정상 교체되는지.
2. **Virtual dtor 커버리지**: `MovingAverage::~MovingAverage = default;` 있고 derived(Ewma/Sma) 가 inherit 하는지 (slicing 방지).
3. **첫 sample 처리**: `tdelta_have_prev_f_` false → 첫 호출에서 방향 안 바꾸는지. 부호 초기값 `tdelta_last_dir_` 검증.
4. **setMovingAverage timing**: createCache 가 ratio 들이 사용되기 전에 호출되는지. 늦게 호출되면 디폴트 EWMA 로 update 가 새 인스턴스로 옮겨가지 않는다.
5. **t_delta state 의존성**: setter 가 ratio 만 교체. `tdelta_prev_f_`, `tdelta_have_prev_f_`, `tdelta_last_dir_` 도 같이 reset 해야 하는지 확인.
6. **periodic_ghost_delta_gc_nand 의 `current_waf` 곱셈 위치**: 기획상 "WAF 높으면 GC 더 / flush 덜" 이 정확히 LHS 가중인지 재확인.
7. **`(1-u_cur)/(1-u_m)` scaling edge case**: u_m=1.0 또는 u_cur=1.0 일 때 0/0 또는 div-by-zero.
8. **Stale .o ABI 위험**: `emwa.h` 변경했을 때 `emwa.cpp` mtime 안 바뀌면 make 가 skip → vtable 불일치로 SIGSEGV 발생 전례 있음. 이번 세션에서 `make clean && make` 로 해결. 향후 헤더 ABI 변경 시 같은 패턴 반복될 수 있음.

## 9. Run scripts

- `run_ma_sweep.sh <POLICY> <US> <MA_TYPE> <MA_WINDOW>`: r∈{2,4,6,8,10} 5-way 병렬, tag = `${POLICY}_us${US_TAG}_${MA_TYPE}`.
- `chain_ma_sweep.sh`: 3 policies × 2 us × 2 MA = 12 sub-sweeps × 5 r = **60 runs**. SMA window = `segment_size_blocks * 32 = 50,331,648`. EWMA window=0 → LogCache 가 `DEFAULT_HALF_LIFE_IN_BLOCKS` 로 fallback.
