# DOGI → ssd_waf 포팅 결정사항 (확정)

## 목적
DOGI 알고리즘(`/home/sejun000/DOGI`)을 cache_sim 안에 새 cache type `DOGI`로 통합. ZNS / RocksDB / ZenFS 의존성은 stub로 제거. DOGI의 in-memory 알고리즘은 **그대로 복붙** — group_optimizer / mlp_inference / model_train / classifier / freq_features / placement / selection / logstore manager / segment / indexmap.

## 페이퍼 핵심 사실 (FAST'26 fast26-dogi.pdf + supplemental)
- paper 셋업: **128 GiB storage**, 256 MiB segment, OP 10%, 4 KiB block
- **우리 cache (130 GB) ≈ paper (128 GiB)** → paper의 절대값 상수 그대로 써도 의미 보존됨
- DOGI는 fallback 메커니즘 보유 (overall accuracy < 10% → ML 끄고 baseline 동작)
- ML inference는 **128-block batch (512 KiB)** 단위
- ML retrain은 **100 GiB user write 마다** (= 26M blocks)
- Hot block은 ML inference 우회 (Hot Filter latest invalidation time만으로 분류)
- MLP architecture: 6 → 32 → 32 → 10, ReLU, ~1.6K params (코드와 일치)
- Frozen Filter: 1 bit per block, 200M writes 마다 clock clear

## 합의된 config

| 항목 | 값 | 비고 |
|---|---|---|
| `kSegmentBlocks` | **98,304** (= 384 MiB / 4 KiB) | cache_sim default 그대로. DOGI 쪽 변경 |
| `OpRatio` | **0.13636** (= 1/0.88 − 1) | cache_sim `_88` 정책 매칭 |
| `GpThreshold` | **0.12** (= 1 − 0.88) | DOGI GC 트리거 임계 |
| `LogicalSizeGb` | `cache_size / 1.13636 / GiB` | cache_sim init에서 환산 주입 |
| `globalTimestamp` | cache_sim `log_cache_timestamp`와 동일 단위 (1 tick = 4 KiB) | manager 내부 카운터 그대로 사용 |
| `kIntervalUnit` (classifier) | **65,536 절대값 고정** | paper 130K 가정. cache 130GB ≈ paper 128GB라 그대로 OK. segment 크기 의존성 끊음 |
| freq_features 상수들 | 코드 default 그대로 | paper 128GB 가정값. 우리 cache 130GB ≈ 동일 |
| `NumGroup` | **6** (Manager 생성 시 고정) | 동적 변경 메커니즘 없음. group_optimizer는 BIR/m_list만 변경 |

## 통합 전략

### 코드 mirror (그대로 복붙)
- `dogi/app/`: classifier, freq_features, group_config, group_optimizer, mlp_inference, model_train, global
- `dogi/src/placement/`: placement.h, factory.h, dogi.{cc,h}, metadata.h
- `dogi/src/selection/`: selection.h, factory.h, dogiselect.{cc,h}, costbenefit.{cc,h}, greedy.{cc,h}
- `dogi/src/logstore/`: manager.{cc,h}, segment.{cc,h}, config.h
- `dogi/src/indexmap/`: indexmap.h, factory.h, hashmap.{cc,h}, array.{cc,h}, filter.{cc,h}
- `dogi/src/storage_adapter/`: storage_adapter.h, factory.h, local_adapter.{cc,h} (local만 살림)
- `DOGI-Train/model_trainer.py`: 서브프로세스로 호출

### 제외
- `app/main.cc` — trace replay 루프, dogi_cache가 대체
- `src/storage_adapter/zenfs_adapter.{cc,h}` — RocksDB/ZenFS/zbd 의존
- `src/buse/*` — block device interface, 사용 안 함
- `src/logstore/logstore.{cc,h}` — buse 의존, dogi_cache가 직접 Manager 인스턴스화
- `src/logstore/scheduler.{cc,h}` — worker thread 모델, dogi_cache가 select/collect 인라인
- 모든 `CMakeLists.txt`

### 신규 파일
- `dogi/storage_adapter/null_adapter.{h,cc}` (~30줄, no-op StorageAdapter)
- `dogi_cache.{h,cc}` (~300줄, ICache → Manager 어댑터)

## ZNS / 스레딩 / 의존성 격리 (Explore 검증 완료)
- `manager.cc`는 ZenFS/RocksDB/zbd 헤더 직접 인클루드 0개 → null adapter만 있으면 컴파일됨
- `LogStore` (buse 의존) 사용 안 함 → buse 의존성 0
- `Scheduler` 사용 안 함 → worker thread / detach / mShutdown 무관
- `Selection`은 dogi_cache에서 직접 `SelectionFactory::GetInstance(Config::GetInstance().selection)` 로 인스턴스화
- DOGI Manager의 `mStorageAdapter->Read/Write/CreateSegment/DestroySegment/ReadWholeSegment` 호출은 모두 null adapter가 no-op 처리
- `ReadSegment → GcAppend` 경로의 buf는 storage I/O용. blockAddr/phyAddr 메타데이터는 buf와 무관 → null adapter OK

## 포팅 시 챙겨야 하는 것 (paper 정독 결과)
모두 **DOGI 코드에 이미 구현되어 있음** — 복붙으로 자동 포함:
- Fallback (acc<10% → APPLY_ML=0): group_optimizer가 토글
- 128-block batch inference: main.cc의 BufferSlot 로직 → **dogi_cache로 이식 필요** (원래 boundary 수정 작업에 포함)
- 100 GiB retrain trigger: model_train.cc의 sample 수 자동 트리거
- Hot block ML 우회: placement/dogi.cc의 Classify 분기

## GC 통합 방식 (inline)
```cpp
// dogi_cache::write 내부
manager.Append(nullptr, lba * 4096, group);
while (manager.GetGp() >= GpThreshold) {
    // scheduler.cc의 select/collect 로직 인라인
    std::vector<Segment> segs;
    manager.GetSegments(segs);
    int sid = mSelection->Select(segs)[0].second;
    Segment seg = manager.ReadSegment(sid);
    manager.CollectSegment(seg.GetSegmentId());
    uint64_t nRewrite = 0;
    for (uint64_t i = 0; i < kSegmentBlocks; ++i) {
        off64_t blk = seg.GetBlockAddr(i);
        if (blk == UINT32_MAX) continue;
        char* d = seg.GetBlockData(i);
        if (!manager.GcAppend(d, blk, seg.GetPhyAddr(i))) ++nRewrite;
    }
    manager.RemoveSegment(seg.GetSegmentId(), seg.GetTotalInvalidBlocks() + nRewrite);
}
```

## 작업 단계
1. ✅ port_dogi.md 결정사항 확정
2. ⏳ DOGI 파일 mirror 복사 → `ssd_waf/dogi/`
3. ⏳ `dogi/storage_adapter/null_adapter.{h,cc}` 작성, factory에 등록
4. ⏳ `dogi/app/global.cc` 의 상수 값 조정 (kSegmentBlocks=98304, OpRatio=0.13636, GpThreshold=0.12, kIntervalUnit=65536). LogicalSizeGb / NumGroup은 dogi_cache init에서 setter
5. ⏳ `dogi_cache.{h,cc}` 신규 작성
6. ⏳ `icache.cpp` 에 `"DOGI"` 분기 추가
7. ⏳ `Makefile` 에 dogi 소스 / `-I dogi/` / `-lopenblas` 추가
8. ⏳ 빌드 → 컴파일/링크 에러 수정
9. ⏳ baseline (`LOG_GREEDY_88` 등)과 동일 trace로 비교 실행

## 위험도: 낮음
- group_optimizer 등 알고리즘 코드 한 줄도 안 건드림
- 잔여 위험: cache_sim segment count / OP / cache_size 매핑 일관성, batch buffering 통합 정확성
