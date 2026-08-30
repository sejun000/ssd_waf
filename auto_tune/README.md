# auto_tune/

Self-contained autotuner for the `LOG_GREEDY_COST_BENEFIT_10_GC_AUTO` policy.
Goal: pick four GC hyperparameters at runtime instead of hardcoding them.

## Decision variables (4D arm)

| Knob | Symbol | Default | Source of default |
| --- | --- | --- | --- |
| Hill-climb direction | `dir`            | `+1`           | `tdelta_last_dir_` initial |
| EWMA half-life       | `half_life_blk`  | `6,291,456`    | `DEFAULT_HALF_LIFE_IN_BLOCKS` |
| Compare period       | `window_segs`    | `8.0`          | hardcoded `segment_size_blocks * 8` |
| Step magnitude       | `step_pct`       | `0.02`         | CLI `--util_step` default |

Round 0 uses `DefaultArm()` so the autotuner is byte-identical to the stock
GC policy until the first reward is observed.

## Reward

```
reward = -(periodic_ratio_r * flush_rate + comp_rate)
       = -(r * evicted_blocks/host_blocks + comp_blocks/host_blocks)
```

Measured over a host-write window (`RoundScheduler`, default 1 TiB).

## ML stack

* Gaussian Process regressor with RBF/ARD kernel (per-dimension length scales).
* Cholesky factorization of `K + sigma_n^2 I`, hand-rolled (N <= ~32 per run,
  no external linalg needed).
* UCB acquisition `mu + beta * sigma` over the 360-arm Cartesian grid.
* No dependency outside the standard library — keeps the build self-contained.

Hyperparameters (`gp_tuner.cc`):

| Parameter | Value | Reason |
| --- | --- | --- |
| `kEll[0]` (dir)            | 1.0 | range = 2 |
| `kEll[1]` (log2 HL ratio)  | 2.0 | range = 5 |
| `kEll[2]` (log2 win ratio) | 2.0 | range = 5 |
| `kEll[3]` (step ratio)     | 2.0 | range ~ 4 |
| `kSigmaF2`                 | 1.0 | rewards are O(1) |
| `kSigmaN2`                 | 1e-3 | f is measured, not noisy |
| `kBetaUCB`                 | 0.5 | exploit-favored (was 2.0; explore-heavy with 360 arms vs <20 obs) |

## Architecture

```
LogCache  ──observe()──▶  GpTuner  ──close_round_and_select()──▶  Arm
                              │
                              └─ X_ (feature), Y_ (reward) ──▶ GpImpl
                                                                 │
                                                                 ├─ fit:  Cholesky
                                                                 └─ acq:  UCB scan
                                                                          over arm_grid_
```

* `arm_grid.h`        — 4D Cartesian grid (2 × 6 × 6 × 5 = 360 arms) and
                       `ArmToFeature()` log-scale normalization.
* `round_scheduler.h` — host-write threshold ticker.
* `gp_tuner.h/.cc`    — GP regressor + UCB. Eigen-free; PIMPL keeps the
                       implementation off the public header.

## Build status

* Phase 1 (folder + headers + skeleton)            — done.
* Phase 2 (GP + UCB, hand-rolled, std-only)        — done.
* Phase 3 (`log_cache` integration)                — pending.
* Phase 4 (`Makefile` updates)                     — pending.

Compile-test (one-off, no Makefile entry yet):

```
g++ -std=c++17 -Wall -Wextra -Wpedantic -O2 -c \
    -Iauto_tune auto_tune/gp_tuner.cc -o /tmp/gp_tuner.o
```

## Synthetic smoke test

Drove `GpTuner` with `f = (hl_ratio - 0.5)^2 + 0.3*(win/8 - 1)^2 + 0.2*(step/0.02 - 1)^2`
plus a small dir penalty. Result: round 2 explores a far corner, sees 58x
worse reward, snaps back; by round 8 the active arm is at the synthetic
optimum (`dir=+1, hl=3,145,728, win=8, step=0.02`) and stays there with
mild exploration. UCB exploration/exploitation curve looks correct.

## Hooks into LogCache (planned, not yet wired)

* New `PeriodicMode::GhostDelta_GC_AUTO` enum.
* New `LogCache::periodic_ghost_delta_gc_auto()` mirroring the stock
  `periodic_ghost_delta_gc()`. The new function (a) calls
  `tuner_->observe(...)` at the sample period, (b) on
  `tuner_->round_closed_since_last_call()` calls
  `close_round_and_select()` and applies the returned arm via
  `setMovingAverage(...)`, `setUtilStep(...)`, plus new dir/window
  knobs (TBD on log_cache side — current setters cover HL via
  `setMovingAverage` and step via `setUtilStep`; need new setters for
  hill-climb direction and compare-window-in-segments).
* New cache_policy string `LOG_GREEDY_COST_BENEFIT_10_GC_AUTO` in
  `icache.cpp::createCache`.
* `Makefile`: add `auto_tune/gp_tuner.cc` to `SRCS_cache_sim`.
