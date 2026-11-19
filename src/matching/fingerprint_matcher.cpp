#include "fingerprint_matcher.h"

#include <algorithm>
#include <cstdlib>

namespace fingerprint {

double compareFeatures(const Features& enrolled, const Features& probe)
{
    int difference = 0;
    for (std::size_t i = 0; i < kFeatureCount; ++i) {
        difference += std::abs(enrolled[i] - probe[i]);
    }

    constexpr int maxDifference = 2 * 255;
    return 1.0 - static_cast<double>(std::min(difference, maxDifference)) / maxDifference;
}

} // namespace fingerprint
