// sma.h — Simple Moving Average over a rolling block-weighted window
#pragma once
#include "emwa.h"
#include <deque>
#include <utility>

class Sma : public MovingAverage {
public:
    explicit Sma(double window_blocks);

    void   updateWithBlocks(double x, double written_blocks) override;
    double value() const override;
    bool   has_value() const override { return sum_w_ > 0.0; }
    void   reset() override;

    double window() const { return window_blocks_; }

private:
    double window_blocks_;
    std::deque<std::pair<double, double>> samples_;  // (value, weight)
    double sum_xw_ = 0.0;
    double sum_w_  = 0.0;
};
