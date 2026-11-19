#pragma once

#include <array>
#include <cstddef>

namespace fingerprint {

// Temporary brightness descriptor shared by extraction and comparison.
inline constexpr std::size_t kFeatureCount = 16;
using Features = std::array<unsigned char, kFeatureCount>;

} // namespace fingerprint
