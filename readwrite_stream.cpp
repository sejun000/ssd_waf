#include "readwrite_stream.h"
#include "multi_hot_cold.h"  // g_stream_cycles, g_cycle_length
#include <cstring>
#include <cstdio>

ReadWriteStream::ReadWriteStream(bool separate_read, int max_gc_streams, int timestamp_granularity,
                                 bool circular, uint64_t coldest_age_threshold)
    : separate_read_(separate_read),
      max_gc_streams_(max_gc_streams),
      timestamp_granularity_(timestamp_granularity),
      circular_(circular),
      coldest_age_threshold_(coldest_age_threshold)
{
    circular_streams_ = (coldest_age_threshold_ > 0) ? max_gc_streams_ - 1 : max_gc_streams_;
    std::memset(stream_cycles_, -1, sizeof(stream_cycles_));
    if (circular_) {
        g_cycle_length = (uint64_t)timestamp_granularity_ * circular_streams_;
    }
}

int ReadWriteStream::classifyCircular(uint64_t created_timestamp, uint64_t global_timestamp, bool is_read) {
    // coldest stream: age >= threshold → dedicated stream
    if (coldest_age_threshold_ > 0 && global_timestamp > created_timestamp &&
        global_timestamp - created_timestamp >= coldest_age_threshold_) {
        return Segment::GC_STREAM_START + circular_streams_;  // slot right after circular streams
    }

    uint64_t age = (global_timestamp > created_timestamp) ? (global_timestamp - created_timestamp) : 0;
    int raw_id = age / timestamp_granularity_;
    int cycle = raw_id / circular_streams_;
    int stream_id = raw_id % circular_streams_;

    // read cold streams are offset by max_gc_streams_
    int base = is_read ? max_gc_streams_ : 0;
    int actual_id = base + stream_id;

    // Detect cycle wrap and update g_stream_cycles
    if (stream_cycles_[actual_id] >= 0 && cycle > stream_cycles_[actual_id]) {
        pending_victim_streams_.push_back(actual_id);
        stream_cycles_[actual_id] = cycle;
        g_stream_cycles[actual_id] = cycle;
    }
    if (stream_cycles_[actual_id] < 0) {
        stream_cycles_[actual_id] = cycle;
        g_stream_cycles[actual_id] = cycle;
    }

    return actual_id + Segment::GC_STREAM_START;
}

int ReadWriteStream::Classify(uint64_t blockAddr, bool isGcAppend,
                               uint64_t global_timestamp,
                               uint64_t created_timestamp, bool is_read) {
    /* Host append: hot/cold by avg lifespan */
    uint64_t lifespan = global_timestamp - created_timestamp;
    if (!isGcAppend) {
        int cls;
        if (lifespan != 0 && lifespan < avg_lifespan_) {
            cls = 0; // hot
        } else {
            cls = 1; // cold
        }
        prev_class_[blockAddr] = cls;
        return cls;
    }

    /* GC append: read */
    if (separate_read_ && is_read) {
        if (circular_) {
            return classifyCircular(created_timestamp, global_timestamp, true);
        }
        return Segment::GC_STREAM_START + 1; // read stream
    }

    /* GC append: write → circular cycle-based sub-streams */
    if (circular_) {
        return classifyCircular(created_timestamp, global_timestamp, false);
    }
    return Segment::GC_STREAM_START; // write stream
}

int ReadWriteStream::GetVictimStreamId(uint64_t global_timestamp, uint64_t threshold) {
    if (!circular_) return -1;

    if (!pending_victim_streams_.empty()) {
        int id = pending_victim_streams_.back();
        pending_victim_streams_.pop_back();
        return id + Segment::GC_STREAM_START;
    }
    return -1;
}

void ReadWriteStream::Append(uint64_t blockAddr, uint64_t global_timestamp, void *arg) {}
void ReadWriteStream::GcAppend(uint64_t blockAddr) {}

void ReadWriteStream::CollectSegment(Segment *segment, uint64_t global_timestamp) {
    static uint64_t totLifespan = 0;
    static int nCollects = 0;
    if (segment->get_class_num() == 0) {
        totLifespan += global_timestamp - segment->get_create_time();
        nCollects += 1;
    }
    if (nCollects == 16) {
        avg_lifespan_ = 1.0 * totLifespan / nCollects;
        nCollects = 0;
        totLifespan = 0;
    }
}
