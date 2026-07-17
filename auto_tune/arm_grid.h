// auto_tune/arm_grid.h
// 1D candidate grid for the LOG_GREEDY_COST_BENEFIT_10_GC_AUTO policy.
// The GP regressor over reward = -f sees a single normalized input.
//
// Decision variable:
//   signed_step : signed offset added to (global_valid/total) to set
//                 target_valid_blk_rate every periodic decision tick.
//                 Positive = raise GC pressure (compact toward higher
//                 valid-rate); negative = lower it (more eviction).
//                 Replaces the prior (dir, util_step) hill-climb knobs.
//
// Defaults: signed_step = +0.02 — matches the in-tree --util_step default and
// reproduces a single raise-by-step at round 0 while GP has no observations.
#pragma once
#include <array>
#include <cstdint>
#include <vector>

namespace auto_tune {

struct Arm {
    double signed_step;  // signed target offset, e.g. +0.02 or -0.04
};

constexpr double kDefaultSignedStep = 0.02;

// 11 arms — symmetric around 0, log-spaced magnitudes mirroring the prior
// step_pct grid (0.005, 0.01, 0.02, 0.04, 0.08).
inline const std::array<double, 11> kSignedStepGrid = {
    -0.08, -0.04, -0.02, -0.01, -0.005,
    0.0,
    +0.005, +0.01, +0.02, +0.04, +0.08,
};

inline Arm DefaultArm() {
    return Arm{kDefaultSignedStep};
}

inline std::vector<Arm> EnumerateAllArms() {
    std::vector<Arm> arms;
    arms.reserve(kSignedStepGrid.size());
    for (double s : kSignedStepGrid) arms.push_back(Arm{s});
    return arms;
}

// Map an arm to a 1D normalized feature vector for kernel/GP input.
// Normalize by the prior default step (0.02) so the feature spans ~[-4, +4].
inline std::array<double, 1> ArmToFeature(const Arm& a) {
    return { a.signed_step / kDefaultSignedStep };
}

} // namespace auto_tune
