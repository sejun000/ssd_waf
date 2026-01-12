#include "midas_model.h"
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <limits>
#include <boost/math/special_functions/lambert_w.hpp>

namespace midas_sim {

MiniModel* model_create(uint64_t logsize_pages, uint32_t interval_unit_size_pages, uint32_t total_segments) {
    auto *m = new MiniModel;
    m->interval_unit_size = interval_unit_size_pages;
    m->time_window = logsize_pages / interval_unit_size_pages;
    if (m->time_window == 0) m->time_window = 1;
    m->real_tw = m->time_window * m->interval_unit_size;
    m->entry_num = static_cast<uint32_t>(m->time_window);
    m->lba_space = logsize_pages;
    m->total_segments = total_segments;
    m->time_stamp.assign(logsize_pages / m->lba_sampling_ratio + 1, UINT64_MAX);
    m->model_count.assign(m->entry_num, 0);
    m->mq = new ModelQ;
    model_reset(m);
    return m;
}

void model_reset(MiniModel* m) {
    std::fill(m->time_stamp.begin(), m->time_stamp.end(), UINT64_MAX);
    std::fill(m->model_count.begin(), m->model_count.end(), 0);
    m->first_count = 0;
    m->first_interval = 0;
    m->total_count = 0;
    m->no_access_lba = 0;
    m->model_on = true;
    m->ginfo.valid = false;
    m->ginfo.gnum = 0;
    m->ginfo.gsize.clear();
    m->ginfo.vr.clear();
    m->current_time = 0;
    m->request_time = 0;
}

void model_destroy(MiniModel* m) {
    if (!m) return;
    delete m->mq;
    delete m;
}

void model_add_g0_sample(MiniModel* m, double traffic_ratio, double valid_ratio, double size_seg) {
    auto &mq = *m->mq;
    if ((int)mq.g0_traffic_queue.size() >= mq.queue_max) mq.g0_traffic_queue.pop_back();
    if ((int)mq.g0_valid_queue.size() >= mq.queue_max) mq.g0_valid_queue.pop_back();
    if ((int)mq.g0_size_queue.size() >= mq.queue_max) mq.g0_size_queue.pop_back();
    mq.g0_traffic_queue.push_front(traffic_ratio);
    mq.g0_valid_queue.push_front(valid_ratio);
    mq.g0_size_queue.push_front(size_seg);
}

double waf_predict(const std::vector<double>& vr, double g0_traffic) {
    size_t gnum = vr.size();
    if (gnum == 0) return 1.0;
    // lambert_w based FIFO estimation for single group shortcut
    if (gnum == 1) {
        double op = 1.0 - vr[0];
        if (op <= 0.0) return 1.0;
        double w = boost::math::lambert_w0(-op * std::exp(-op));
        return op / (op + w);
    }
    size_t N = gnum + 1; // include hot slot
    std::vector<double> val2(N + 1, 0.0);
    val2[0] = 1.0;
    val2[1] = vr[0];
    for (size_t i = 1; i < gnum; ++i) val2[i + 1] = vr[i];

    std::vector<std::vector<double>> mtx(N + 1, std::vector<double>(N + 1, 0.0));
    for (size_t i = 0; i <= N; ++i) {
        mtx[i][0] = 1.0 - val2[i];
        if (i == 0) {
            mtx[i][1] = g0_traffic;
            mtx[i][2] = 1.0 - mtx[i][1];
        } else if (i == N) {
            mtx[i][i] = val2[i];
        } else {
            mtx[i][i + 1] = val2[i];
        }
    }
    std::vector<double> base(N + 1, 0.0), res(N + 1, 0.0);
    base[0] = 1000.0;
    base[1] = 9000.0;
    res = base;
    double total_wr = 0.0;
    double tmp_tot_wr = 0.0;
    double past = 0.0;
    for (int iter = 0; iter < 2000; ++iter) {
        tmp_tot_wr = 0.0;
        base.swap(res);
        std::fill(res.begin(), res.end(), 0.0);
        for (size_t j = 0; j <= N; ++j) {
            for (size_t k = 0; k <= N; ++k) {
                res[j] += base[k] * mtx[k][j];
            }
        }
        if (gnum == 1) {
            total_wr += res[2] - res[0] * val2[1];
            tmp_tot_wr += res[2] - res[0] * val2[1];
        } else {
            for (size_t j = 0; j < gnum - 1; ++j) {
                total_wr += res[j + 3];
                tmp_tot_wr += res[j + 3];
            }
            total_wr += res[1] * val2[1];
            tmp_tot_wr += res[1] * val2[1];
        }
        past = res[0];
    }
    if (past == 0) return 1.0;
    double WAF = (tmp_tot_wr + past) / past;
    return WAF;
}

bool model_tick(MiniModel* m, uint32_t pages) {
    if (!m || !m->model_on) return false;
    for (uint32_t i = 0; i < pages; ++i) {
        m->request_time++;
        if (m->request_time >= m->interval_unit_size) {
            m->request_time = 0;
            m->current_time++;
            if (m->current_time >= m->time_window) {
                m->model_on = false;
                return true; // epoch finished
            }
        }
    }
    return false;
}

void model_update_lba(MiniModel* m, uint64_t lba) {
    if (!m || !m->model_on) return;
    if (m->lba_sampling_ratio == 0) return;
    if (lba % m->lba_sampling_ratio) return;
    lba = lba / m->lba_sampling_ratio;
    if (lba >= m->time_stamp.size()) return;
    uint64_t tmp_time = m->current_time * m->interval_unit_size + m->request_time;
    if (m->time_stamp[lba] == UINT64_MAX || m->time_stamp[lba] == UINT64_MAX-1) {
        m->time_stamp[lba] = tmp_time << 1;
        return;
    }
    uint64_t prev = m->time_stamp[lba] >> 1;
    uint64_t interval = (tmp_time > prev) ? (tmp_time - prev) / m->interval_unit_size : 0;
    if (interval >= m->entry_num) interval = m->entry_num - 1;
    m->model_count[interval] += 1;
    m->time_stamp[lba] = tmp_time << 1;
}

std::vector<double> predict_valid_ratio(const MiniModel* m, int gnum) {
    std::vector<double> vr(gnum, 0.8);
    if (!m || gnum <= 0 || m->model_count.empty()) return vr;
    double total = 0.0;
    for (auto c : m->model_count) total += (double)c;
    if (total == 0.0) return vr;
    // cumulative distribution over interval buckets
    std::vector<double> cdf(m->model_count.size(), 0.0);
    double running = 0.0;
    for (size_t i = 0; i < m->model_count.size(); ++i) {
        running += (double)m->model_count[i];
        cdf[i] = running / total;
    }
    for (int g = 0; g < gnum; ++g) {
        double target = (double)(g + 1) / (double)(gnum + 1);
        size_t idx = 0;
        while (idx < cdf.size() && cdf[idx] < target) idx++;
        double interval_norm = (double)idx / (double)std::max<uint32_t>(1, m->entry_num);
        double est_vr = std::exp(- (interval_norm + 0.001));
        vr[g] = std::min(0.99, std::max(0.01, est_vr));
    }
    // ensure monotonic non-decreasing valid ratios towards cold
    for (int g = 1; g < gnum; ++g) {
        if (vr[g] < vr[g-1]) vr[g] = vr[g-1] + 0.01;
        if (vr[g] > 0.99) vr[g] = 0.99;
    }
    return vr;
}

void model_finalize(MiniModel* m, int target_groups) {
    if (!m) return;
    auto &g = m->ginfo;
    int best_gnum = std::max<int>(2, target_groups);
    double best_waf = std::numeric_limits<double>::max();
    std::vector<uint32_t> best_gsize;
    std::vector<double> best_vr;

    // derive mean interval for vr estimation
    double sum_cnt = 0.0;
    double sum_w = 0.0;
    for (size_t i = 0; i < m->model_count.size(); ++i) {
        double cnt = (double)m->model_count[i];
        sum_cnt += cnt;
        sum_w += cnt * (double)i;
    }
    double mean_interval = (sum_cnt > 0) ? (sum_w / sum_cnt) : (m->entry_num * 0.5);

    uint64_t tot_seg = m->total_segments ? m->total_segments : (uint64_t)target_groups;
    if (tot_seg == 0) tot_seg = target_groups;

    int max_groups = std::max(2, target_groups);
    int upper = std::min(6, max_groups + 2);
    double g0_traffic = 0.1;
    auto &mq = *m->mq;
    if (!mq.g0_traffic_queue.empty()) {
        double t=0.0;
        for (auto v: mq.g0_traffic_queue) t+=v;
        g0_traffic = t / (double)mq.g0_traffic_queue.size();
    }

    for (int cand = 2; cand <= upper; ++cand) {
        size_t n = (size_t)cand;
        std::vector<double> vr = predict_valid_ratio(m, (int)n);
        std::vector<uint32_t> gsize(n, 1);

        // size proportional to (1 - vr)
        double wsum = 0.0;
        std::vector<double> w(n, 1.0);
        for (size_t i = 0; i < n; ++i) {
            w[i] = 1.0 - vr[i];
            if (w[i] < 0.01) w[i] = 0.01;
            wsum += w[i];
        }
        uint64_t assigned = 0;
        for (size_t i = 0; i < n; ++i) {
            uint32_t sz = (uint32_t)std::max<double>(1.0, std::floor(w[i]/wsum * tot_seg));
            gsize[i] = sz;
            assigned += sz;
        }
        while (assigned < tot_seg) { gsize.back()++; assigned++; }
        while (assigned > tot_seg && gsize.back() > 1) { gsize.back()--; assigned--; }

        double waf = waf_predict(vr, g0_traffic);
        if (waf < best_waf) {
            best_waf = waf;
            best_gnum = cand;
            best_gsize = gsize;
            best_vr = vr;
        }
    }

    g.gnum = best_gnum;
    g.gsize = best_gsize;
    g.vr = best_vr;
    g.WAF = best_waf;
    g.valid = true;
}

void model_add_segment_sample(MiniModel* m, int group_id, double valid_ratio, double seg_blocks) {
    if (!m) return;
    if (group_id == 0) {
        model_add_g0_sample(m, 0.0, valid_ratio, seg_blocks);
    }
}

void model_finish_epoch(MiniModel* m) {
    model_finalize(m, m->ginfo.gnum > 0 ? m->ginfo.gnum : 2);
}

} // namespace midas_sim
