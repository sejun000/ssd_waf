#include "midas_hf.h"

namespace midas_sim {

HotFilter hf_create(long lba_space, int max_val, int hot_val) {
    HotFilter hf;
    hf.max_val = max_val;
    hf.hot_val = hot_val;
    if (lba_space > 0) hf.cur_hf.reserve(static_cast<std::size_t>(lba_space));
    hf.tw = lba_space > 0 ? lba_space : 0;
    hf.left_tw = hf.tw;
    hf.cold_tw = hf.tw;
    return hf;
}

void hf_reset(HotFilter &hf, int flag) {
    hf.cur_hf.clear();
    hf.left_tw = hf.tw;
    hf.cold_tw = hf.tw;
    if (flag == 1) hf.make_flag = 1;
    hf.use_flag = 0;
}

void hf_metadata_reset(HotFilter &hf) {
    hf.use_flag = 0;
    hf.make_flag = 0;
    hf.ready_flag = 0;
    hf.G0_vr_sum = 0;
    hf.G0_vr_num = 0;
    hf.seg_age = 0;
    hf.seg_num = 0;
    hf.avg_seg_age = 0;
    hf.G0_traffic_ratio = 0;
    hf.tot_traffic = 0;
    hf.G0_traffic = 0;
    hf.hot_lba_num = 0;
    hf.valid_lba_num = 0;
    hf.left_tw = hf.tw;
    hf.cold_tw = hf.tw;
    hf.err_cnt = 0;
    hf.hf_cnt = 0;
    hf.tmp_err_cnt = 0;
}

void hf_generate(HotFilter &hf, long lba, bool hflag) {
    if (!hf.make_flag) return;
    if (hf.cold_tw > 0) {
        hf.cold_tw--;
        return;
    }

    if (hflag) {
        hf.left_tw--;
    }

    int &cnt = hf.cur_hf[lba];
    if (cnt < hf.max_val) cnt++;
    if (cnt >= hf.hot_val) hf.hot_lba_num++;
    hf.tot_traffic++;
}

int hf_check(const HotFilter &hf, long lba) {
    if (!hf.use_flag) return 1; // cold
    auto it = hf.cur_hf.find(lba);
    if (it == hf.cur_hf.end()) return 1;
    return (it->second >= hf.hot_val) ? 0 : 1;
}

} // namespace midas_sim
