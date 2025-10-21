#include "istream.h"
#include "sepbit.h"
#include "hot_cold.h"
#include "multi_hot_cold.h"
#include <string>
#include <cassert>

//uint64_t interval = 8000000;
uint64_t interval = 10000000;

IStream* createIstreamPolicy(std::string policy_type) {
    if (policy_type == "sepbit") {
        return new SepBIT();
    }
    else if (policy_type == "hotcold") {
        return new HotCold();
    } else if (policy_type == "multi_hotcold") {
        return new MultiHotCold(20, interval, false);
    }
    else if (policy_type == "multi_hotcold_create_timestamp_only") {
        return new MultiHotCold(20, interval, true);
    }
    else if (policy_type == "multi_hotcold_2") {
        return new MultiHotCold(20, interval, true, true, false);
    }
    else if (policy_type == "multi_hotcold_3") {
        return new MultiHotCold(20, interval, true, true, true);
    }
    else {
        assert(false);
    }
}