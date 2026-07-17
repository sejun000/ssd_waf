/*───────────────────────────────────────────────────────*/
/* cb_evict_policy.cpp                                   */
/*───────────────────────────────────────────────────────*/
#include "evict_policy_cost_benefit.h"
#include <cassert>
#include <cstdlib>
#include <vector>
#include <algorithm>

// Definition for the extern declared in evict_policy.h. See header for details.
double g_ghost_v_override = -1.0;

CbEvictPolicy::CbEvictPolicy(double (*func)(Segment* )) {
    score_func = func;
}

void CbEvictPolicy::add(Segment* s)
{
    assert(s);
    if (h_.count(s)) return;  // Already in heap, skip duplicate add
    auto h = heap_.push({ score(s), s });
    h_[s]  = h;
}

void CbEvictPolicy::remove(Segment* s)
{
    auto it = h_.find(s);
    if (it == h_.end()) return;
    heap_.erase(it->second);
    h_.erase(it);
}

void CbEvictPolicy::update(Segment* s)
{
    auto it = h_.find(s);
    if (it == h_.end()) return;            // 이미 제거된 경우
    heap_.update(it->second, { score(s), s });
}

Segment* CbEvictPolicy::choose_segment()
{
    size_t initial_size = heap_.size();

    /* top‑k (K_VALIDATE) 노드 재평가로 순위 확정 */
    for (int i = 0; i < K_VALIDATE && !heap_.empty(); ++i) {
        CBNode top = heap_.top();
        double cur = score(top.seg);
        if (cur == top.score) {
            remove(top.seg);
            return top.seg;        // 여전히 1등
        }
        heap_.pop();                                 // 순위 달라짐
        auto h = heap_.push({ cur, top.seg });       // 재삽입
        h_[top.seg] = h;
    }

    // K_VALIDATE 반복 후에도 결정 안 됨 - 현재 top 선택
    if (heap_.empty()) {
        fprintf(stderr, "CbEvictPolicy::choose_segment: heap empty! initial_size=%zu\n", initial_size);
        assert(false);
    }
    Segment* seg = heap_.top().seg;
    remove(seg);
    return seg;
}

uint64_t CbEvictPolicy::get_mth_score_valid_pages(double m) const
{
    if (m <= 0.0 || heap_.empty()) return 0;

    int full = static_cast<int>(m);        // floor(m)
    double frac = m - full;                // fractional part
    uint64_t sum = 0;
    auto it = heap_.ordered_begin();
    for (int i = 0; i < full && it != heap_.ordered_end(); ++i, ++it) {
        sum += it->seg->valid_cnt;
    }
    if (frac > 0.0 && it != heap_.ordered_end()) {
        sum += static_cast<uint64_t>(it->seg->valid_cnt * frac);
    }
    return sum;
}

uint64_t CbEvictPolicy::get_kth_segment_valid_cnt_for_free_segments(double m) const
{
    if (m <= 0.0 || heap_.empty()) return 0;

    double free_sum = 0.0;
    uint64_t last_valid_cnt = 0;

    // score 순서(max→min)로 순회, (1-U_i) 누적하여 m 이상이 되면 마지막 segment의 valid_cnt 반환
    for (auto it = heap_.ordered_begin(); it != heap_.ordered_end(); ++it) {
        double u_i = static_cast<double>(it->seg->valid_cnt) / pages_in_segment;
        free_sum += (1.0 - u_i);
        last_valid_cnt = it->seg->valid_cnt;
        if (free_sum >= m) break;
    }

    return last_valid_cnt;
}

EvictPolicy::GhostSumResult
CbEvictPolicy::get_ghost_sum_for_free_segments(double target_free_segments,
                                                uint64_t now)
{
    GhostSumResult r;
    if (target_free_segments <= 0.0 || heap_.empty()) return r;

    /* Top-K(=K_VALIDATE) 재정렬 — stored score 는 stale 하다(score_warm_first 의
     * warmth=min(thr-age,age) 가 age 의존인데 g_timestamp 는 계속 흐르고, 해당 seg 가
     * 최근 invalidate 안 되면 score 갱신 안 됨). top-1 만 고치면 아래 누적이 2~3 깊이로
     * 내려가며 stale-상위에 낀 high-u seg 를 끌어와 Gud 를 부풀린다. choose_segment 와
     * 같은 K window 의 상위 노드를 모두 현재 score 로 refresh → ghost walk 순서가 실제
     * victim 순서와 일치하고 Gud 가 진짜 최저비용 victim 을 반영. */
    {
        std::vector<Segment*> topk;
        topk.reserve(K_VALIDATE);
        int cnt = 0;
        for (auto it = heap_.ordered_begin();
             it != heap_.ordered_end() && cnt < K_VALIDATE; ++it, ++cnt)
            topk.push_back(it->seg);
        for (Segment* s : topk) update(s);   // re-score & reposition
    }

    // inv_rate-based corrections to v (GUD_GAP/TOPK/RESORT) removed: Gud is now the
    // raw valid-page sum G(u+δ). The natural-death credit lives entirely on the
    // other side of the rule as r·invrate_sum (computed independently in
    // periodic_ghost_delta_gc_sum_final), so it is no longer folded into Gud here.

    double free_sum = 0.0;
    int picked_idx = 0;   // record first 4 picks for diagnostic logging
    for (auto it = heap_.ordered_begin(); it != heap_.ordered_end(); ++it) {
        // Record create_timestamp + u of first 4 ghost picks (scan order)
        if (picked_idx < 4) {
            r.picked_create_ts[picked_idx] =
                static_cast<int64_t>(it->seg->get_create_time());
            r.picked_u[picked_idx] =
                static_cast<double>(it->seg->valid_cnt) /
                static_cast<double>(pages_in_segment);
            ++picked_idx;
        }
        const double v   = static_cast<double>(it->seg->valid_cnt);  // raw valid (no inv_rate correction)
        const double inv = static_cast<double>(pages_in_segment) - v;
        const double inv_frac = inv / static_cast<double>(pages_in_segment);
        const int cn = it->seg->class_num;
        const int cidx = (cn >= 0 && cn < 16) ? cn : 15;  // out-of-range → slot 15 ("other")
        const double inv_rate = it->seg->invalidate_rate();
        // Raw last-fold inv_rate, computed on-the-fly without state mutation:
        // (count - prev_count) / (now - prev_ts).
        double inv_rate_lf = 0.0;
        if (now > 0 && it->seg->invalidate_prev_ts_ > 0 &&
            now > it->seg->invalidate_prev_ts_) {
            const uint64_t dU = it->seg->invalidate_count_ - it->seg->invalidate_prev_count_;
            const uint64_t dH = now - it->seg->invalidate_prev_ts_;
            inv_rate_lf = static_cast<double>(dU) / static_cast<double>(dH);
        }
        if (free_sum + inv_frac >= target_free_segments) {
            const double need = target_free_segments - free_sum;
            const double frac = (inv > 0.0) ? (need * static_cast<double>(pages_in_segment) / inv)
                                            : 0.0;
            r.cum_valid   += v   * frac;
            r.cum_invalid += inv * frac;
            r.m           += frac;
            r.m_by_class[cidx] += frac;
            r.sum_invalidate_rate    += inv_rate    * frac;
            r.sum_inv_rate_last_fold += inv_rate_lf * frac;
            break;
        }
        free_sum      += inv_frac;
        r.cum_valid   += v;
        r.cum_invalid += inv;
        r.m           += 1.0;
        r.m_by_class[cidx] += 1.0;
        r.sum_invalidate_rate    += inv_rate;
        r.sum_inv_rate_last_fold += inv_rate_lf;
    }
    return r;
}

EvictPolicy::VictimWtSpanResult
CbEvictPolicy::get_victim_wt_span_for_free_segments(double target_free_segments) const
{
    VictimWtSpanResult r;
    if (target_free_segments <= 0.0 || heap_.empty()) return r;

    double free_sum = 0.0;
    for (auto it = heap_.ordered_begin(); it != heap_.ordered_end(); ++it) {
        Segment* s = it->seg;
        const uint64_t v   = s->valid_cnt;
        const uint64_t inv = (pages_in_segment > v) ? (pages_in_segment - v) : 0;
        const double inv_frac = static_cast<double>(inv) / static_cast<double>(pages_in_segment);

        // Segment is part of the cleaned set → track the newest WT (victim v).
        const uint64_t wt = s->get_create_time();
        if (r.v_seg == nullptr || wt > r.max_wt) { r.max_wt = wt; r.v_seg = s; }
        r.m += 1.0;

        if (free_sum + inv_frac >= target_free_segments) break;  // target reached
        free_sum += inv_frac;
    }
    return r;
}

void CbEvictPolicy::for_each_victim_in_order(const std::function<bool(Segment*)>& fn) const
{
    // max→min score order == the order choose_segment() would hand out victims.
    for (auto it = heap_.ordered_begin(); it != heap_.ordered_end(); ++it) {
        if (!fn(it->seg)) break;
    }
}
