#include "template_manager.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fingerprint {
namespace {

// Magic, little-endian format version 1, and little-endian dimension 512.
constexpr unsigned char kDeepPrintHeader[] = {'D', 'P', 'T', 'M', 1, 0, 0, 2};

} // namespace

int encodeTemplate(const Features& features, unsigned char* output)
{
#ifdef FINGERPRINT_DEEPPRINT
    constexpr auto payloadSize = kFeatureCount * sizeof(float);
    constexpr auto templateSize = sizeof(kDeepPrintHeader) + payloadSize;
    static_assert(sizeof(float) == 4 && kFeatureCount == 512, "DeepPrint template format");
    static_assert(templateSize < 1000000, "KISA template size limit");
    std::memcpy(output, kDeepPrintHeader, sizeof(kDeepPrintHeader));
    std::memcpy(output + sizeof(kDeepPrintHeader), features.data(), payloadSize);
    return static_cast<int>(templateSize);
#else
    std::copy(features.begin(), features.end(), output);
    return static_cast<int>(features.size());
#endif
}

bool decodeTemplate(const unsigned char* input, Features& features)
{
#ifdef FINGERPRINT_DEEPPRINT
    if (std::memcmp(input, kDeepPrintHeader, sizeof(kDeepPrintHeader)) != 0) {
        return false;
    }
    std::memcpy(features.data(), input + sizeof(kDeepPrintHeader), kFeatureCount * sizeof(float));
    return std::all_of(features.begin(), features.end(), [](float value) {
        return std::isfinite(value) && std::abs(value) <= 1.001f;
    }) && std::any_of(features.begin(), features.end(), [](float value) { return value != 0.0f; });
#else
    if (std::memcmp(input, kDeepPrintHeader, 4) == 0) {
        return false;
    }
    std::copy_n(input, features.size(), features.begin());
    return true;
#endif
}

} // namespace fingerprint
