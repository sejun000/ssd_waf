#pragma once
#include <unordered_map>
#include <cstdint>

namespace midas_sim {

struct HotFilter {
    int max_val = 3;
    int hot_val = 3;
    double tw_ratio = 1.0;
    int make_flag = 1;   // enable generation by default
    int use_flag = 1;    // enable usage by default
    int ready_flag = 0;

    long tw = 0;
    long left_tw = 0;
    long cold_tw = 0;

    double G0_vr_sum = 0;
    double G0_vr_num = 0;

    double seg_age = 0;
    double seg_num = 0;
    double avg_seg_age = 0;

    double G0_traffic_ratio = 0;
    double tot_traffic = 0;
    double G0_traffic = 0;
    int hot_lba_num = 0;
    double valid_lba_num = 0;

    int err_cnt = 0;
    int hf_cnt = 0;
    int tmp_err_cnt = 0;

    std::unordered_map<long, int> cur_hf;
};

// Simplified initialization: lba_space is the expected key population (for reserve only)
HotFilter hf_create(long lba_space, int max_val = 3, int hot_val = 3);
void hf_reset(HotFilter &hf, int flag = 0);
void hf_metadata_reset(HotFilter &hf);
void hf_generate(HotFilter &hf, long lba, bool hflag);
int  hf_check(const HotFilter &hf, long lba);

} // namespace midas_sim
