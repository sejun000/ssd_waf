// auto_tune/gp_tuner.cc
// Hand-rolled GP regressor + UCB acquisition for the GC-AUTO policy.
// No external linalg dependency: training-set N <= ~32 (a 14 TiB trace at
// 1 TiB/round caps the round count), so a textbook Cholesky on an N x N
// matrix is fine — way cheaper than wiring Eigen into the build.

#include "gp_tuner.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace auto_tune {

namespace {

// RBF (squared-exponential) kernel on the 1D signed-step feature.
//   dim 0 (signed_step / 0.02)   — range ~[-4, +4],   ell = 2.0
// ell=2.0 means arms within ±2 default-step magnitudes share information;
// extremes (±0.08 → feat ±4) are weakly correlated to mid (±0.01 → feat ±0.5).
constexpr double kEll[1]  = {2.0};
constexpr double kSigmaF2 = 1.0;     // signal variance (rewards are O(1))
constexpr double kSigmaN2 = 1e-3;    // observation noise (f is measured)
constexpr double kBetaUCB = 0.5;     // UCB exploration weight

inline double rbf(const std::array<double, 1>& a,
                  const std::array<double, 1>& b) {
    const double t = (a[0] - b[0]) / kEll[0];
    return kSigmaF2 * std::exp(-0.5 * t * t);
}

// In-place Cholesky: A := L where A = L * L^T, L lower-triangular.
// Adds a tiny jitter to the diagonal for numerical safety on near-singular
// kernel matrices (happens when arms repeat).
void cholesky(std::vector<std::vector<double>>& A) {
    const int n = static_cast<int>(A.size());
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j <= i; ++j) {
            double s = A[i][j];
            for (int k = 0; k < j; ++k) s -= A[i][k] * A[j][k];
            if (i == j) {
                A[i][i] = std::sqrt(std::max(s, 1e-12));
            } else {
                A[i][j] = s / A[j][j];
            }
        }
        for (int j = i + 1; j < n; ++j) A[i][j] = 0.0;
    }
}

// Solve L x = b   (L lower-triangular, stored in-place in `L`).
void forward_solve(const std::vector<std::vector<double>>& L,
                   const std::vector<double>& b,
                   std::vector<double>& x) {
    const int n = static_cast<int>(L.size());
    x.assign(n, 0.0);
    for (int i = 0; i < n; ++i) {
        double s = b[i];
        for (int j = 0; j < i; ++j) s -= L[i][j] * x[j];
        x[i] = s / L[i][i];
    }
}

// Solve L^T x = b   (uses transposed access on the same storage).
void backward_solve(const std::vector<std::vector<double>>& L,
                    const std::vector<double>& b,
                    std::vector<double>& x) {
    const int n = static_cast<int>(L.size());
    x.assign(n, 0.0);
    for (int i = n - 1; i >= 0; --i) {
        double s = b[i];
        for (int j = i + 1; j < n; ++j) s -= L[j][i] * x[j];
        x[i] = s / L[i][i];
    }
}

}  // namespace

struct GpTuner::GpImpl {
    // Cached after fit():  L = chol(K + sigma_n^2 I),   alpha = (K + sigma_n^2 I)^-1 y.
    std::vector<std::vector<double>> L;
    std::vector<double>              alpha;
    double                           y_mean = 0.0;
    bool                             ready  = false;

    void fit(const std::vector<std::array<double, 1>>& X,
             const std::vector<double>&                Y) {
        const int n = static_cast<int>(X.size());
        if (n == 0) { ready = false; return; }
        // Center the targets so the GP zero-mean prior is meaningful.
        double sum = 0.0;
        for (double v : Y) sum += v;
        y_mean = sum / n;
        std::vector<double> Yc(n);
        for (int i = 0; i < n; ++i) Yc[i] = Y[i] - y_mean;

        std::vector<std::vector<double>> K(n, std::vector<double>(n, 0.0));
        for (int i = 0; i < n; ++i) {
            for (int j = 0; j <= i; ++j) {
                double v = rbf(X[i], X[j]);
                if (i == j) v += kSigmaN2;
                K[i][j] = v;
                K[j][i] = v;
            }
        }
        cholesky(K);
        L = std::move(K);
        std::vector<double> z;
        forward_solve(L, Yc, z);
        backward_solve(L, z, alpha);
        ready = true;
    }

    // Predict mu, sigma at x_star given training inputs X (must match the
    // X passed to fit()).
    void predict(const std::array<double, 1>&                x_star,
                 const std::vector<std::array<double, 1>>&   X,
                 double&                                     mu,
                 double&                                     sigma) const {
        if (!ready || X.empty()) {
            mu    = 0.0;
            sigma = std::sqrt(kSigmaF2);
            return;
        }
        const int n = static_cast<int>(X.size());
        std::vector<double> kvec(n);
        for (int i = 0; i < n; ++i) kvec[i] = rbf(X[i], x_star);

        // Posterior mean: mu = y_mean + k_*^T alpha
        double m = y_mean;
        for (int i = 0; i < n; ++i) m += kvec[i] * alpha[i];

        // Posterior variance: var = k(x*,x*) - v^T v   where  L v = k_*
        std::vector<double> v;
        forward_solve(L, kvec, v);
        double var = rbf(x_star, x_star);
        for (int i = 0; i < n; ++i) var -= v[i] * v[i];
        if (var < 1e-12) var = 1e-12;

        mu    = m;
        sigma = std::sqrt(var);
    }
};

GpTuner::GpTuner(double   periodic_ratio_r,
                 uint64_t bytes_per_round,
                 uint64_t seed)
    : r_weight_(periodic_ratio_r),
      active_(DefaultArm()),
      sched_(bytes_per_round),
      arm_grid_(EnumerateAllArms()),
      rng_(seed),
      gp_(std::make_unique<GpImpl>()) {}

GpTuner::~GpTuner() = default;

Arm GpTuner::initial_arm() const { return DefaultArm(); }

void GpTuner::observe(uint64_t cum_host_bytes,
                      uint64_t cum_evict_blocks,
                      uint64_t cum_comp_blocks) {
    if (round_index_ == 0 && latest_host_bytes_ == 0) {
        round_start_host_bytes_ = cum_host_bytes;
        round_start_evict_blk_  = cum_evict_blocks;
        round_start_comp_blk_   = cum_comp_blocks;
    }
    latest_host_bytes_ = cum_host_bytes;
    latest_evict_blk_  = cum_evict_blocks;
    latest_comp_blk_   = cum_comp_blocks;
    if (sched_.tick(cum_host_bytes)) round_just_closed_ = true;
}

bool GpTuner::round_closed_since_last_call() {
    if (!round_just_closed_) return false;
    round_just_closed_ = false;
    return true;
}

Arm GpTuner::close_round_and_select() {
    constexpr uint64_t kBlockBytes = 4096;
    const uint64_t host_delta = latest_host_bytes_ - round_start_host_bytes_;
    const uint64_t ev_delta   = latest_evict_blk_  - round_start_evict_blk_;
    const uint64_t cp_delta   = latest_comp_blk_   - round_start_comp_blk_;
    const double   host_blocks = static_cast<double>(host_delta) / kBlockBytes;
    if (host_blocks > 0.0) {
        const double flush = static_cast<double>(ev_delta) / host_blocks;
        const double comp  = static_cast<double>(cp_delta) / host_blocks;
        const double f     = r_weight_ * flush + comp;
        last_flush_rate_   = flush;
        last_comp_rate_    = comp;
        last_reward_       = -f;
        X_.push_back(ArmToFeature(active_));
        Y_.push_back(last_reward_);
    }

    // Snapshot for the next round.
    round_start_host_bytes_ = latest_host_bytes_;
    round_start_evict_blk_  = latest_evict_blk_;
    round_start_comp_blk_   = latest_comp_blk_;
    ++round_index_;

    // UCB acquisition over arm_grid_.
    if (X_.empty()) {
        active_     = DefaultArm();
        last_mu_    = 0.0;
        last_sigma_ = std::sqrt(kSigmaF2);
        return active_;
    }
    gp_->fit(X_, Y_);
    Arm    best       = arm_grid_.front();
    double best_acq   = -std::numeric_limits<double>::infinity();
    double best_mu    = 0.0;
    double best_sigma = 0.0;
    for (const Arm& a : arm_grid_) {
        const auto feat = ArmToFeature(a);
        double mu, sigma;
        gp_->predict(feat, X_, mu, sigma);
        const double acq = mu + kBetaUCB * sigma;
        if (acq > best_acq) {
            best_acq   = acq;
            best       = a;
            best_mu    = mu;
            best_sigma = sigma;
        }
    }
    active_     = best;
    last_mu_    = best_mu;
    last_sigma_ = best_sigma;
    return active_;
}

void GpTuner::set_arm_grid(std::vector<Arm> arms) {
    arm_grid_ = std::move(arms);
}

}  // namespace auto_tune
