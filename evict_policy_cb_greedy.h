#pragma once
#include "evict_policy.h"
#include <boost/heap/d_ary_heap.hpp>
#include <unordered_map>
#include "segment.h"

struct CbgNode {
    double   score;
    Segment* seg;
    bool operator<(const CbgNode& o) const noexcept {
        return score < o.score || (score == o.score && seg < o.seg);
    }
};

struct GreedyNode {
    int64_t  neg_valid;  // -valid_cnt for max-heap (lowest valid_cnt = highest priority)
    Segment* seg;
    bool operator<(const GreedyNode& o) const noexcept {
        return neg_valid < o.neg_valid || (neg_valid == o.neg_valid && seg < o.seg);
    }
};

using CbgHeap = boost::heap::d_ary_heap<CbgNode, boost::heap::arity<4>, boost::heap::mutable_<true>>;
using GrdHeap = boost::heap::d_ary_heap<GreedyNode, boost::heap::arity<4>, boost::heap::mutable_<true>>;

class CbGreedyEvictPolicy final : public EvictPolicy {
public:
    CbGreedyEvictPolicy(double (*func)(Segment*));
    Segment* choose_segment() override;
    void add(Segment* seg) override;
    void remove(Segment* seg) override;
    void update(Segment* seg) override;
    bool empty() const override { return cb_heap_.empty() && grd_heap_.empty(); }
    size_t segment_count() const override { return cb_heap_.size(); }

private:
    double (*score_func_)(Segment*);
    static constexpr int K_VALIDATE = 10;

    CbgHeap cb_heap_;
    std::unordered_map<Segment*, CbgHeap::handle_type> cb_h_;

    GrdHeap grd_heap_;
    std::unordered_map<Segment*, GrdHeap::handle_type> grd_h_;

    inline double cb_score(Segment* s) const { return score_func_(s); }
};
