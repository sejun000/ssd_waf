// auto_tune/gp_tuner.h
// Public interface for the Gaussian-Process-based autotuner used by the
// LOG_GREEDY_COST_BENEFIT_10_GC_AUTO policy.
//
// Lifecycle from the simulator side:
//   GpTuner tuner(periodic_ratio_r);
//   Arm a = tuner.initial_arm();              // == DefaultArm() on round 0
//   ... apply a (HL/window/step/dir) to LogCache ...
//   per host write: tuner.observe(host_blocks, evict_blocks, comp_blocks);
//   on round close: Arm next = tuner.close_round_and_select();
//                   ... apply next ...
//
// All ML internals (kernel, posterior, acquisition) live in gp_tuner.cc.
// Eigen will be the linalg backend; this header stays Eigen-free so callers
// don't need to know about it.
#pragma once
#include "arm_grid.h"
#include "round_scheduler.h"
#include <cstdint>
#include <memory>
#include <random>
#include <vector>

namespace auto_tune {

class GpTuner {
public:
    // periodic_ratio_r is the f-weight (= --periodic_ratio CLI). The reward
    // posted to the GP is -(r*flush + comp) measured over the round window.
    explicit GpTuner(double periodic_ratio_r,
                     uint64_t bytes_per_round = (1ull << 40),
                     uint64_t seed = 0xC0FFEE);
    ~GpTuner();

    // Round 0 always returns DefaultArm() so the autotuner reproduces the
    // stock GC policy until the first reward is observed.
    Arm initial_arm() const;

    // Per-block accumulator. Cumulative inputs (host bytes, evict blocks,
    // compacted blocks) — caller passes the LogCache totals; GpTuner takes
    // the delta against the round-start snapshot internally.
    void observe(uint64_t cum_host_bytes,
                 uint64_t cum_evict_blocks,
                 uint64_t cum_comp_blocks);

    // Returns true once the active round has closed (RoundScheduler tick).
    // Must be called by the simulator at a frequency at least as fine as the
    // round size (host bytes); cheap (no allocations).
    bool round_closed_since_last_call();

    // Combine: call after round_closed_since_last_call() returns true.
    // Reports reward for the active arm and selects the next one.
    Arm close_round_and_select();

    // Optional overrides for grid + scheduler.
    void set_arm_grid(std::vector<Arm> arms);
    void set_round_bytes(uint64_t v) { sched_.set_bytes_per_round(v); }

    // Diagnostics (for stat logging).
    uint64_t round_index() const { return round_index_; }
    Arm      active_arm()  const { return active_; }
    double   last_reward() const { return last_reward_; }
    size_t   train_size()  const { return Y_.size(); }
    double   last_flush_rate() const { return last_flush_rate_; }
    double   last_comp_rate()  const { return last_comp_rate_; }
    double   last_mu()         const { return last_mu_; }
    double   last_sigma()      const { return last_sigma_; }

private:
    // ---- accumulators reset every round ----
    uint64_t round_start_host_bytes_  = 0;
    uint64_t round_start_evict_blk_   = 0;
    uint64_t round_start_comp_blk_    = 0;
    uint64_t latest_host_bytes_       = 0;
    uint64_t latest_evict_blk_        = 0;
    uint64_t latest_comp_blk_         = 0;
    bool     round_just_closed_       = false;

    // ---- tuner state ----
    double             r_weight_;        // f = r*flush + comp
    Arm                active_;          // arm currently in effect
    uint64_t           round_index_     = 0;
    double             last_reward_     = 0.0;
    double             last_flush_rate_ = 0.0;
    double             last_comp_rate_  = 0.0;
    double             last_mu_         = 0.0;  // GP posterior mean of selected arm
    double             last_sigma_      = 0.0;  // GP posterior stddev of selected arm
    RoundScheduler     sched_;
    std::vector<Arm>   arm_grid_;
    std::vector<std::array<double, 1>> X_;   // observed features
    std::vector<double>                Y_;   // observed rewards (= -f)
    std::mt19937_64                    rng_;

    // GP backend lives in the .cc to keep Eigen out of the public header.
    struct GpImpl;
    std::unique_ptr<GpImpl> gp_;

    // No copy.
    GpTuner(const GpTuner&) = delete;
    GpTuner& operator=(const GpTuner&) = delete;
};

} // namespace auto_tune
