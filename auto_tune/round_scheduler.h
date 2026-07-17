// auto_tune/round_scheduler.h
// Decides when an autotuning round closes (so the GP can score the active arm
// and pick the next one). The default unit is "host write bytes" because the
// policy comparison metric f = r*flush + comp is naturally measured per host
// write window.
#pragma once
#include <cstdint>

namespace auto_tune {

class RoundScheduler {
public:
    // bytes_per_round defaults to 1 TiB to match the granularity of the offline
    // sweeps (the f-objective stabilizes around that scale on this trace).
    explicit RoundScheduler(uint64_t bytes_per_round = (1ull << 40))
        : bytes_per_round_(bytes_per_round) {}

    // Returns true if the new cumulative host-write counter has crossed the
    // next round boundary since the last call. Idempotent for non-monotone
    // input is undefined — the caller must feed monotonically growing values.
    bool tick(uint64_t cumulative_host_write_bytes) {
        if (cumulative_host_write_bytes >= next_threshold_) {
            // Catch up if multiple rounds elapsed in one tick.
            while (cumulative_host_write_bytes >= next_threshold_) {
                next_threshold_ += bytes_per_round_;
            }
            return true;
        }
        return false;
    }

    uint64_t bytes_per_round() const { return bytes_per_round_; }
    void     set_bytes_per_round(uint64_t v) { bytes_per_round_ = v; }

private:
    uint64_t bytes_per_round_;
    uint64_t next_threshold_ = (1ull << 40); // first round closes at 1 TiB
};

} // namespace auto_tune
