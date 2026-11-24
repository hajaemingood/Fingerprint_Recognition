#pragma once

#include <array>
#include <cstddef>

namespace fingerprint {

#ifdef FINGERPRINT_DEEPPRINT
inline constexpr std::size_t kFeatureCount = 512;
using Features = std::array<float, kFeatureCount>;
#else
inline constexpr std::size_t kFeatureCount = 16;
using Features = std::array<unsigned char, kFeatureCount>;
#endif

} // namespace fingerprint
