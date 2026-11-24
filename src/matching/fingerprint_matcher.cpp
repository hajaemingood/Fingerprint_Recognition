#include "fingerprint_matcher.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace fingerprint {

double compareFeatures(const Features& enrolled, const Features& probe)
{
#ifdef FINGERPRINT_DEEPPRINT
    double dot = 0.0;
    double enrolledNorm = 0.0;
    double probeNorm = 0.0;
    for (std::size_t i = 0; i < kFeatureCount; ++i) {
        const double a = enrolled[i];
        const double b = probe[i];
        dot += a * b;
        enrolledNorm += a * a;
        probeNorm += b * b;
    }

    const double cosine = dot / std::sqrt(enrolledNorm * probeNorm);
    return (std::clamp(cosine, -1.0, 1.0) + 1.0) / 2.0;
#else
    int difference = 0;
    for (std::size_t i = 0; i < kFeatureCount; ++i) {
        difference += std::abs(enrolled[i] - probe[i]);
    }

    constexpr int maxDifference = 2 * 255;
    return 1.0 - static_cast<double>(std::min(difference, maxDifference)) / maxDifference;
#endif
}

} // namespace fingerprint
