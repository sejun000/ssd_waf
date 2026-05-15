// sma.cpp
#include "sma.h"
#include <stdexcept>

Sma::Sma(double window_blocks) : window_blocks_(window_blocks) {
    if (!(window_blocks > 0.0)) {
        throw std::invalid_argument("Sma window_blocks must be > 0");
    }
}

void Sma::updateWithBlocks(double x, double written_blocks) {
    if (!(written_blocks > 0.0)) return;
    samples_.emplace_back(x, written_blocks);
    sum_xw_ += x * written_blocks;
    sum_w_  += written_blocks;

    // Trim front while excess weight is fully past the window.
    while (!samples_.empty() && sum_w_ - samples_.front().second >= window_blocks_) {
        auto& f = samples_.front();
        sum_xw_ -= f.first * f.second;
        sum_w_  -= f.second;
        samples_.pop_front();
    }
    // Partial trim of front: shrink its weight so total weight ≤ window.
    if (!samples_.empty() && sum_w_ > window_blocks_) {
        auto& f = samples_.front();
        double over = sum_w_ - window_blocks_;
        double new_w = f.second - over;
        if (new_w <= 0.0) {
            sum_xw_ -= f.first * f.second;
            sum_w_  -= f.second;
            samples_.pop_front();
        } else {
            sum_xw_ -= f.first * over;
            sum_w_  -= over;
            f.second = new_w;
        }
    }
}

double Sma::value() const {
    if (sum_w_ <= 0.0) return 0.0;
    return sum_xw_ / sum_w_;
}

void Sma::reset() {
    samples_.clear();
    sum_xw_ = 0.0;
    sum_w_  = 0.0;
}
