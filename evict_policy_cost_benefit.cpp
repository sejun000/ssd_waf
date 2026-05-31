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

    /* choose_segment() 와 동일한 K_VALIDATE 재평가 — heap top 의 stored score 가
     * stale 일 수 있어서 top 이 stable 해질 때까지 (또는 K 번까지) re-score & re-push.
     * 그래야 ghost 가 보는 victim 순서가 실제 choose_segment 가 뽑는 순서와 일치. */
    for (int i = 0; i < K_VALIDATE && !heap_.empty(); ++i) {
        CBNode top = heap_.top();
        double cur = score(top.seg);
        if (cur == top.score) break;   // top stable
        heap_.pop();
        auto h = heap_.push({ cur, top.seg });
        h_[top.seg] = h;
    }

    // Optional invalidate-gap correction (env GUD_GAP_CORRECT=1). The predicted GC
    // does not run instantly: each victim is compacted only after host writes have
    // consumed the free space GC freed ahead of it (≈ free_sum·seg host-write pages).
    // During that wait the victim sheds ≈ invalidate_rate·H of its valid pages, so GC
    // copies fewer (cum_valid↓) and frees more (cum_invalid↑). invalidate_rate's denom
    // is the same global host clock as H → units match (inval/host·host = pages).
    static const bool gap_correct = [](){
        const char* e = std::getenv("GUD_GAP_CORRECT");
        return e && *e == '1';
    }();
    // Optional top-K invalidation-concentration correction (env GUD_TOPK_CORRECT=1).
    // Hypothesis: invalidations concentrate on a few hot segments during the 1-seg
    // window between RAISE and the next compaction tick. Those hot segments become
    // new top-1 candidates that ghost (frozen at RAISE) didn't anticipate. Proxy:
    // measure the past-1-seg's top-K observed inv_rates across ALL heap segments
    // (sorted desc) → apply top_k_rates[i] × seg_blocks to discount the i-th ghost
    // pick's valid. Captures "if these picks were among the concentrated receivers".
    static const bool topk_correct = [](){
        const char* e = std::getenv("GUD_TOPK_CORRECT");
        return e && *e == '1';
    }();
    // Re-sort top-N candidates after per-segment self-inv-rate correction
    // (env GUD_RESORT_CORRECT=1). Each candidate's v is virtually advanced by
    // 1 seg of its own past observed inv_rate (= simulating next 1 seg of host
    // writes) — then re-rank by score(v_corr). Captures the cohort drift
    // mechanism: a segment receiving heavy invalidation has u drop, score=age/u
    // rise, becomes the new top-1. Standard scan then runs on the re-sorted set.
    static const bool resort_correct = [](){
        const char* e = std::getenv("GUD_RESORT_CORRECT");
        return e && *e == '1';
    }();
    static const int resort_top_n = [](){
        const char* e = std::getenv("GUD_RESORT_N");
        if (e && *e) {
            const int n = std::atoi(e);
            if (n > 0) return n;
        }
        return 20;
    }();

    if (resort_correct && now > 0 && !heap_.empty()) {
        struct Item {
            double   score;
            double   v_corr;
            Segment* seg;
            int      cidx;
            double   inv_rate;
            double   inv_rate_lf;
        };
        std::vector<Item> items;
        items.reserve(static_cast<size_t>(resort_top_n));
        int taken = 0;
        for (auto it = heap_.ordered_begin();
             it != heap_.ordered_end() && taken < resort_top_n; ++it, ++taken) {
            Segment* s = it->seg;
            double rate_lf = 0.0;
            if (s->invalidate_seg_ago_ts_ > 0 &&
                s->invalidate_prev_ts_ > s->invalidate_seg_ago_ts_) {
                const uint64_t dU = s->invalidate_prev_count_ - s->invalidate_seg_ago_count_;
                const uint64_t dH = s->invalidate_prev_ts_ - s->invalidate_seg_ago_ts_;
                rate_lf = static_cast<double>(dU) / static_cast<double>(dH);
            }
            double v_corr = static_cast<double>(s->valid_cnt)
                          - rate_lf * static_cast<double>(pages_in_segment);
            if (v_corr < 0.0) v_corr = 0.0;
            // Re-score with corrected v via g_ghost_v_override (read by score_*
            // funcs and CbEvictPolicy::score). No segment-state mutation —
            // override is set/cleared around the single score() call below.
            g_ghost_v_override = v_corr;
            const double sc = score(s);
            g_ghost_v_override = -1.0;
            const int cn = s->class_num;
            const int cidx_local = (cn >= 0 && cn < 16) ? cn : 15;
            items.push_back({sc, v_corr, s, cidx_local,
                             s->invalidate_rate(), rate_lf});
        }
        std::sort(items.begin(), items.end(),
                  [](const Item& a, const Item& b){ return a.score > b.score; });
        double free_sum_rs = 0.0;
        int picked_idx_rs = 0;
        for (const auto& it : items) {
            const uint64_t pick_wt = it.seg->get_create_time();
            if (pick_wt > r.max_picked_wt) r.max_picked_wt = pick_wt;
            if (picked_idx_rs < 4) {
                r.picked_create_ts[picked_idx_rs] =
                    static_cast<int64_t>(pick_wt);
                r.picked_u[picked_idx_rs] =
                    it.v_corr / static_cast<double>(pages_in_segment);
                ++picked_idx_rs;
            }
            const double v   = it.v_corr;
            const double inv = static_cast<double>(pages_in_segment) - v;
            const double inv_frac = inv / static_cast<double>(pages_in_segment);
            if (free_sum_rs + inv_frac >= target_free_segments) {
                const double need = target_free_segments - free_sum_rs;
                const double frac = (inv > 0.0)
                    ? (need * static_cast<double>(pages_in_segment) / inv) : 0.0;
                r.cum_valid   += v   * frac;
                r.cum_invalid += inv * frac;
                r.m           += frac;
                r.m_by_class[it.cidx] += frac;
                r.sum_invalidate_rate    += it.inv_rate    * frac;
                r.sum_inv_rate_last_fold += it.inv_rate_lf * frac;
                return r;
            }
            free_sum_rs   += inv_frac;
            r.cum_valid   += v;
            r.cum_invalid += inv;
            r.m           += 1.0;
            r.m_by_class[it.cidx] += 1.0;
            r.sum_invalidate_rate    += it.inv_rate;
            r.sum_inv_rate_last_fold += it.inv_rate_lf;
        }
        return r;
    }

    constexpr int TOPK = 8;
    double top_k_rates[TOPK] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    if (topk_correct && now > 0) {
        // Pre-scan: extract each segment's just-closed 1-seg-window raw inv_rate
        // (= (prev_count − seg_ago_count) / (prev_ts − seg_ago_ts)). Closed window
        // matches what real_inv_rate_lf measures on the realized victim side.
        // Maintain top-K sorted descending via insertion (K=8 → cheap).
        for (auto it = heap_.ordered_begin(); it != heap_.ordered_end(); ++it) {
            const Segment* s = it->seg;
            double r = 0.0;
            if (s->invalidate_seg_ago_ts_ > 0 &&
                s->invalidate_prev_ts_ > s->invalidate_seg_ago_ts_) {
                const uint64_t dU = s->invalidate_prev_count_ - s->invalidate_seg_ago_count_;
                const uint64_t dH = s->invalidate_prev_ts_ - s->invalidate_seg_ago_ts_;
                r = static_cast<double>(dU) / static_cast<double>(dH);
            }
            for (int k = 0; k < TOPK; ++k) {
                if (r > top_k_rates[k]) {
                    for (int j = TOPK - 1; j > k; --j) top_k_rates[j] = top_k_rates[j-1];
                    top_k_rates[k] = r;
                    break;
                }
            }
        }
        for (int k = 0; k < TOPK; ++k) r.top_k_rates[k] = top_k_rates[k];
    }

    double free_sum = 0.0;
    int picked_idx = 0;   // record first 4 picks for diagnostic logging
    int corr_idx = 0;     // top-K correction index (separate from picked_idx 4-cap)
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
        double v = static_cast<double>(it->seg->valid_cnt);
        if (gap_correct) {
            const double H_i = free_sum * static_cast<double>(pages_in_segment);  // host pages before this victim
            v -= it->seg->invalidate_rate() * H_i;                                // pages that die in the gap
            if (v < 0.0) v = 0.0;
        }
        if (topk_correct) {
            // Worst-case shed during next 1 seg: pair i-th pick with i-th hottest
            // observed rate. Beyond TOPK, fall back to the last (=lowest of top-K)
            // rate. Multiplier = seg_blocks (= 1 full seg of host writes ahead).
            const int k_use = (corr_idx < TOPK) ? corr_idx : (TOPK - 1);
            v -= top_k_rates[k_use] * static_cast<double>(pages_in_segment);
            if (v < 0.0) v = 0.0;
        }
        ++corr_idx;
        const double inv = static_cast<double>(pages_in_segment) - v;            // freed (incl. gap deaths)
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
