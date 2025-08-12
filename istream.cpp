#include "istream.h"
#include "sepbit.h"
#include "hot_cold.h"
#include <string>
#include <cassert>

IStream* createIstreamPolicy(std::string policy_type) {
    if (policy_type == "sepbit") {
        return new SepBIT();
    }
    else if (policy_type == "hotcold") {
        return new HotCold();
    } else {
        assert(false);
    }
}