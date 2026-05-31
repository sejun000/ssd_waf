#pragma once
#include <list>
#include <cstdint>
#include <cstddef>
#include <functional>

class Segment;

// Ghost-prediction-only valid_cnt override for re-sort path
// (GUD_RESORT_CORRECT). When >= 0, score_* functions and the default age/u
// score in CbEvictPolicy use this value INSTEAD of seg->valid_cnt — allowing
// "what-if 1 seg of self-invalidation" re-ranking without mutating the live
// segment. Single-threaded simulator, restored to -1 immediately after each
// score call. -1 = disabled (normal read of valid_cnt).
extern double g_ghost_v_override;

class EvictPolicy {
public:
    virtual ~EvictPolicy() = default;

    /* victim 선택 */
    virtual Segment* choose_segment() = 0;
    virtual Segment* choose_segment(bool next_id) {
        return choose_segment();  // 기본 구현은 next_id 여부 무시
    }
    virtual Segment* choose_segment(int stream_id) {
        return choose_segment();  // 기본 구현은 stream_id 무시
    }

    // only for do_evict_and_compaction_with_same_policy
    virtual Segment* choose_segment_for_eviction(bool next_id) {
        return choose_segment();
    }

    virtual Segment* choose_segment_for_compaction(bool next_id) {
        return choose_segment();
    }

    /* 세그먼트 추가 / 제거 */
    virtual void add(Segment* seg)       = 0;
    virtual void add(Segment* seg, uint64_t current_time) { 
        add(seg);  // 기본 구현은 current_time 무시 
    }

    virtual void remove(Segment* seg)    = 0;

    /* valid_cnt 가 변했을 때 호출 */
    virtual void update(Segment* seg)    = 0;
    virtual void update(Segment* seg, uint64_t current_time) {
        update(seg);  // 기본 구현은 current_time 무시
    }

    /* Check if evictor has any segments to choose from */
    virtual bool empty() const { return false; }  // Default: not empty

    /* Get valid_cnt of m-th segment in score order (for ghost compaction estimation) */
    virtual uint64_t get_mth_score_valid_pages(double m) const { return 0; }

    /* m개의 free segment를 확보하려면 k번째 segment까지 compact해야 함
     * k = min(k | sum(i=1..k) (1 - U_i) >= m), return k번째 segment의 valid_cnt */
    virtual uint64_t get_kth_segment_valid_cnt_for_free_segments(double m) const { return 0; }

    /* Cumulative version: scan score-order until reclaimed free space (in
     * segment-equivalent units) reaches `target_free_segments`. Returns the
     * cumulative valid pages and invalid pages over the visited segments and
     * the number of segments visited (m). Used by periodic_ghost_delta_gc_sum
     * to estimate G(u+θ) ≈ Σ valid_i over cleaned-set instead of a single
     * boundary segment. */
    struct GhostSumResult {
        double cum_valid   = 0.0;
        double cum_invalid = 0.0;
        double m           = 0.0;
        // Per-class-num victim count (fractional last victim contributes its `frac`).
        // Index = seg->class_num (capped at 15; user write streams typically 0-2,
        // GC compaction targets at Segment::GC_STREAM_START..+2 = 10-12).
        double m_by_class[16] = {0};
        // Σ invalidate_rate over picked victims (fractional last by `frac`).
        // mean = sum_invalidate_rate / m → 평균 victim 의 최근 invalidation rate.
        double sum_invalidate_rate = 0.0;
        // Σ raw inv_rate (last-fold dU/dH, no EWMA) over picked victims.
        // Computed on-the-fly from invalidate_count_/prev_count_/prev_ts_ at scan
        // time (no state mutation). Requires `now` (current log_cache_timestamp).
        double sum_inv_rate_last_fold = 0.0;
        // create_timestamp of the first 4 picked victims (in scan order). -1 if
        // fewer than 4 picked. Used to compare ghost-picked set against real-
        // compacted set at RAISE diagnostic logging.
        int64_t picked_create_ts[4] = { -1, -1, -1, -1 };
        // u (= valid_cnt / seg_blocks) at scan time for the same 4 picked
        // victims. Direct measurement (no derivation) → lets us compare against
        // real-side u (measured at execute time) for the SAME segment.
        double  picked_u[4]         = { 0.0, 0.0, 0.0, 0.0 };
        // Top-8 raw inv_rates (sorted desc) observed across heap segments over
        // the just-closed 1-seg window. Populated by GUD_TOPK_CORRECT path —
        // used to discount the i-th picked victim's valid by top_k_rates[i] ×
        // seg_blocks (proxy for concentrated-invalidation cohort drift). Zero
        // when correction disabled.
        double  top_k_rates[8]      = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
        // Largest create_timestamp among picked victims (RESORT path) — same
        // role as vspan.max_wt but on the corrected-score-ordered pick set.
        // When RESORT is active and this > 0, log_cache should use this as the
        // cohort boundary for invrate_sum (vs the baseline vspan call which
        // doesn't see RESORT corrections). 0 = not populated (disabled).
        uint64_t max_picked_wt      = 0;
    };
    // non-const: derived class may revalidate top-K heap nodes (re-score & re-push
    // if score changed) before the scan to match choose_segment()'s K_VALIDATE rule.
    // `now` (current log_cache_timestamp): when >0, derived class can compute raw
    // per-fold inv_rate for sum_inv_rate_last_fold. Default 0 = skip raw computation.
    virtual GhostSumResult get_ghost_sum_for_free_segments(double target_free_segments,
                                                            uint64_t now = 0) {
        return {};
    }

    /* Same score-order scan as get_ghost_sum_for_free_segments, but instead of
     * page sums it reports "victim v" — the newest segment (max create_timestamp)
     * GC must touch to free `target_free_segments`.  Boundary segment is folded
     * in whole (WT is atomic). Used as the upper bound of a later WT query. */
    struct VictimWtSpanResult {
        uint64_t max_wt = 0;            // "victim v": largest create_timestamp in cleaned set
        double   m      = 0.0;          // segments folded in (boundary inclusive)
        Segment* v_seg  = nullptr;      // the max-WT victim segment
    };
    virtual VictimWtSpanResult get_victim_wt_span_for_free_segments(double target_free_segments) const {
        return {};
    }

    /* Visit victim segments in selection (score) order.  fn returns false to
     * stop early.  Default no-op for policies without an ordered victim list.
     * Used by what-if GC simulation that needs per-victim segment access
     * (the heap is private to the concrete policy). */
    virtual void for_each_victim_in_order(const std::function<bool(Segment*)>& fn) const {}

    /* Get current segment count in the policy */
    virtual size_t segment_count() const { return 0; }

    void init(uint64_t* time, std::size_t size, int num) {
        logical_time = time;  // 외부에서 logical_time을 설정
        pages_in_segment = size;  // 세그먼트 크기 설정
        segment_num = num;  // 세그먼트 개수 설정
    }
    // protected: // EvictPolicy는 Segment에 대한 접근이 필요하므로 protected로 설정
protected:
    std::size_t pages_in_segment = 0;  // 세그먼트 내의 page수 
    uint64_t segment_num = 0;          // 세그먼트 개수
    uint64_t* logical_time = nullptr;  // 전역 증가하는 logical time
};
