#pragma once
#include <vector>
#include <cstdint>
#include <deque>

namespace midas_sim {

struct GInfo {
    bool valid = false;
    int gnum = 0;
    std::vector<uint32_t> gsize;
    std::vector<double> vr;
    double WAF = 0.0;
    int commit_g = 0;
    double g0_traffic = 0.0;
};

struct ModelQ {
    std::deque<double> g0_traffic_queue;
    std::deque<double> g0_valid_queue;
    std::deque<double> g0_size_queue;
    double g0_traffic = 0.0;
    double g0_valid = 0.0;
    double g0_size = 0.0;
    int queue_max = 10;
};

struct MiniModel {
    uint32_t lba_sampling_ratio = 100;
    uint32_t interval_unit_size = 0;
    uint64_t time_window = 0;
    uint64_t real_tw = 0;
    uint32_t entry_num = 0;
    uint64_t current_time = 0;
    uint32_t request_time = 0;
    uint64_t lba_space = 0;

    uint32_t fnumber = 0;
    std::vector<uint32_t> checking_first_interval;
    uint32_t first_count = 0;
    uint64_t first_interval = 0;

    long no_access_lba = 0;
    uint64_t total_count = 0;
    bool model_on = true;
    int model_idx = 0;

    std::vector<uint64_t> time_stamp;
    std::vector<uint64_t> model_count;

    ModelQ *mq = nullptr;
    GInfo ginfo;
    uint32_t total_segments = 0;
};

MiniModel* model_create(uint64_t logsize_pages, uint32_t interval_unit_size_pages, uint32_t total_segments);
void model_reset(MiniModel* m);
void model_destroy(MiniModel* m);
void model_add_g0_sample(MiniModel* m, double traffic_ratio, double valid_ratio, double size_seg);
void model_add_segment_sample(MiniModel* m, int group_id, double valid_ratio, double seg_blocks = 1.0);
void model_finish_epoch(MiniModel* m);
double waf_predict(const std::vector<double>& vr, double g0_traffic = 0.1);
bool model_tick(MiniModel* m, uint32_t pages = 1);
void model_update_lba(MiniModel* m, uint64_t lba);
void model_finalize(MiniModel* m, int target_groups);
std::vector<double> predict_valid_ratio(const MiniModel* m, int gnum);

} // namespace midas_sim
