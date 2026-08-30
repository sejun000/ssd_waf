# Long-term (200 TB) run configs

> 헷갈림 방지용 레퍼런스. 현재 alibaba marginal-rule 스윕(`run_margcap15_2r.sh`,
> 14 TiB single-pass, `cache_write_size_limit=15393162788864`)과 **다른 실험**이다.
> 여기 "200TB" = **host write 200 TiB = `219902325555200` bytes** 한도로 trace 를
> loop 돌려 long-term steady-state 까지 보던 dwpd 3-tier 실험.
> run.log 헤더에서 그대로 뽑은 값 (source of truth = `dwpd*_h200.run.log`).

## 공통 파라미터 (lo/mid/hi 전부 동일)

| 항목 | 값 |
|---|---|
| cache_size (arg2) | `1883510931456` (1.88 TB) |
| block_size | `4096` |
| trace_format | `csv4col` |
| cache_policy | `LOG_GREEDY_COST_BENEFIT_10_GS_FINAL` |
| cold_capacity | `16050000000000` (16.05 TB) |
| periodic_ratio (r) | `8.64` |
| util_step | `0.02` |
| moving_avg_type / window | `ewma` / `1572864` (= 1 segment, hl1572864) |
| gs_decision_period_segs (d) | `1` |
| remap_lba | **enabled** (4K sequential alloc) |
| loop_trace | **enabled** (EOF 시 자동 재시작) |
| prefill | disabled |

## 트레이스별 차이 (scale 로 working-set/DWPD 맞춤)

| tier | trace (`../` = 부모 dir) | scale | rw_policy |
|---|---|---|---|
| lo  | `dwpd_lo_first14TB_writes.csv`  | `3`  | (미지정 → `all`) |
| mid | `dwpd_mid_first14TB_writes.csv` | `14` | (미지정 → `all`) |
| hi  | `dwpd_hi_first14TB_writes.csv`  | `10` 또는 `18` | `write-only` |

## Write-limit 변종 (200 TB 가 들어가는 두 자리)

- **host 200 TiB (h200, 표준 "200TB씩"):** `--cache_write_size_limit 219902325555200`, cold 한도 disabled. → lo/mid h200 가 이거. (h160 변종 = `175921860444160` = 160 TiB.)
- **cold 200 TiB (hi 계열):** `--cache_write_size_limit 1099511627776000` (1000 TiB, 안 닿게 크게) + `--cold_write_size_limit 219902325555200`. 단, **디스크에 남은 hi run 은 cold=50 TiB(`54975581388800`)** 로 실제 완주(200은 계획값). 1-pass 후 cold 발동시키려면 `--min_trace_loops 1`.
- 200 TiB = `200 * 2^40` = `219902325555200`. 160 TiB=`175921860444160`, 50 TiB=`54975581388800`, 1000 TiB=`1099511627776000`. (run.log 은 TiB 를 "TB" 로 표기함.)

## 표준 커맨드 — lo, host 200 TiB (`dwpdlo_s3_h200`)

```bash
./cache_sim ../dwpd_lo_first14TB_writes.csv 1883510931456 \
    --block_size 4096 --trace_format csv4col \
    --cache_policy LOG_GREEDY_COST_BENEFIT_10_GS_FINAL \
    --cold_capacity 16050000000000 \
    --waf_log_file dwpdlo_s3_h200.waf.log --stat_log_file dwpdlo_s3_h200.stat \
    --cache_trace /mnt/nvme2n2/dwpdlo_s3_h200.trace \
    --cold_trace  /mnt/nvme2n2/dwpdlo_s3_h200.cold.trace \
    --scale 3 --remap_lba --loop_trace \
    --cache_write_size_limit 219902325555200 \
    --periodic_ratio 8.64 --util_step 0.02 \
    --moving_avg_type ewma --moving_avg_window 1572864 \
    --gs_decision_period_segs 1 \
    > dwpdlo_s3_h200.run.log 2>&1 < /dev/null &
```

mid 는 `dwpd_mid...` + `--scale 14`, 태그 `dwpdmid_s14_h200` 로 동일. hi 는 위 "cold 200 TiB" 변종 + `--rw_policy write-only` + `--scale 10|18`.

## 비고

- 출력 trace(`--cache_trace`/`--cold_trace`)는 `/mnt/nvme2n2/` 로 뺐음 (용량). 실측엔 안 쓰면 생략 가능.
- WAF 읽을 땐 stat 말고 `*.waf.log` col4/col3 (cold WAF) — stat 의 ftl_nand 가짜 1.0 버그 주의 (LOG_FIFO 한정이지만 습관).
- LOG_FIFO 베이스라인 변종도 같은 한도로 존재: `dwpd*_h200_logfifo.*`.
