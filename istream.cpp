#include "istream.h"
#include "sepbit.h"
#include <string>
#include <cassert>

IStream* createIstreamPolicy(std::string policy_type) {
    if (policy_type == "sepbit") {
        return new SepBIT();
    } else {
        assert(false);
    }
}