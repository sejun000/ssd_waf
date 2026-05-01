#include "evict_policy_cb_greedy.h"
#include <cassert>

CbGreedyEvictPolicy::CbGreedyEvictPolicy(double (*func)(Segment*))
    : score_func_(func) {}

void CbGreedyEvictPolicy::add(Segment* seg) {
    assert(seg);
    if (cb_h_.count(seg)) return;  // already in both heaps
    auto ch = cb_heap_.push({cb_score(seg), seg});
    cb_h_[seg] = ch;
    auto gh = grd_heap_.push({-static_cast<int64_t>(seg->valid_cnt), seg});
    grd_h_[seg] = gh;
}

void CbGreedyEvictPolicy::remove(Segment* seg) {
    auto it = cb_h_.find(seg);
    if (it != cb_h_.end()) {
        cb_heap_.erase(it->second);
        cb_h_.erase(it);
    }
    auto it2 = grd_h_.find(seg);
    if (it2 != grd_h_.end()) {
        grd_heap_.erase(it2->second);
        grd_h_.erase(it2);
    }
}

void CbGreedyEvictPolicy::update(Segment* seg) {
    auto it = cb_h_.find(seg);
    if (it != cb_h_.end()) {
        cb_heap_.update(it->second, {cb_score(seg), seg});
    }
    auto it2 = grd_h_.find(seg);
    if (it2 != grd_h_.end()) {
        grd_heap_.update(it2->second, {-static_cast<int64_t>(seg->valid_cnt), seg});
    }
}

Segment* CbGreedyEvictPolicy::choose_segment() {
    // Try CB heap first: re-evaluate top K_VALIDATE times
    for (int i = 0; i < K_VALIDATE && !cb_heap_.empty(); ++i) {
        CbgNode top = cb_heap_.top();
        double cur = cb_score(top.seg);
        if (cur > 0 && cur == top.score) {
            // score is positive and stable → pick this
            remove(top.seg);
            return top.seg;
        }
        if (cur > 0) {
            // score changed but still positive → re-insert and retry
            cb_heap_.pop();
            auto ch = cb_heap_.push({cur, top.seg});
            cb_h_[top.seg] = ch;
            continue;
        }
        // score <= 0 → this segment is bad, stop trying CB
        break;
    }

    // CB didn't produce a good candidate → fallback to greedy
    if (!grd_heap_.empty()) {
        Segment* seg = grd_heap_.top().seg;
        remove(seg);
        return seg;
    }

    // Both empty
    assert(!cb_heap_.empty() && "CbGreedyEvictPolicy: both heaps empty");
    // Last resort: take CB top regardless
    Segment* seg = cb_heap_.top().seg;
    remove(seg);
    return seg;
}
