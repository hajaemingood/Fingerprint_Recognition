#include "template_manager.h"

#include <algorithm>

namespace fingerprint {

int encodeTemplate(const Features& features, unsigned char* output)
{
    static_assert(kFeatureCount < 1000000, "KISA template size limit");
    std::copy(features.begin(), features.end(), output);
    return static_cast<int>(features.size());
}

Features decodeTemplate(const unsigned char* input)
{
    Features features{};
    std::copy_n(input, features.size(), features.begin());
    return features;
}

} // namespace fingerprint
