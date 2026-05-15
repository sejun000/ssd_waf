#pragma once
#include "emwa.h"
#include "sma.h"
#include <memory>
#include <string>

class MovingAverageRatio {
public:
    explicit MovingAverageRatio(std::unique_ptr<MovingAverage> ma)
        : ma_(std::move(ma)), prevH_(0), prevUc_(0), initialized_(false) {}

    MovingAverageRatio(MovingAverageRatio&&) = default;
    MovingAverageRatio& operator=(MovingAverageRatio&&) = default;

    // Factory: build by type ("ewma" | "sma") + window (blocks).
    // ewma: window = half-life. sma: window = rolling window length.
    static MovingAverageRatio Make(const std::string& type, double window_blocks) {
        if (type == "sma") {
            return MovingAverageRatio(std::unique_ptr<MovingAverage>(new Sma(window_blocks)));
        }
        // default: ewma (with bias correction)
        return MovingAverageRatio(std::unique_ptr<MovingAverage>(
            new Ewma(Ewma::FromHalfLifeBlocks(window_blocks, /*bias_correction=*/true))));
    }

    // Backward-compat: behaves identically to old EwmaRatio::FromHalfLifeBlocks.
    static MovingAverageRatio FromHalfLifeBlocks(double hl_blocks, bool bias_correction = true) {
        return MovingAverageRatio(std::unique_ptr<MovingAverage>(
            new Ewma(Ewma::FromHalfLifeBlocks(hl_blocks, bias_correction))));
    }

    // 직접 Δ를 주는 버전
    void update(long double numer_inc, long double denom_inc) {
        if (denom_inc <= 0.0L) return;
        double x = static_cast<double>(numer_inc / denom_inc);
        ma_->updateWithBlocks(x, static_cast<double>(denom_inc));
    }

    // 누적값(H, Uc) 넣으면 내부에서 Δ를 계산해 update
    void updateFromCumulative(long double H, long double Uc) {
        if (!initialized_) {
            prevH_  = H;
            prevUc_ = Uc;
            initialized_ = true;
            return;
        }
        long double dH  = H  - prevH_;
        long double dUc = Uc - prevUc_;
        if (dH > 0.0L) {
            prevH_  = H;
            prevUc_ = Uc;
            update(dUc, dH);
        }
    }

    double value() const { return ma_->value(); }
    bool has_value() const { return ma_->has_value(); }

private:
    std::unique_ptr<MovingAverage> ma_;
    long double prevH_, prevUc_;
    bool initialized_;
};

// Backward-compat alias for code that still references the old name.
using EwmaRatio = MovingAverageRatio;
