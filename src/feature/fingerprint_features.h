#pragma once

#include <array>
#include <cstddef>

namespace fingerprint {

inline constexpr std::size_t kFeatureCount = 512;
using Features = std::array<float, kFeatureCount>;

} // namespace fingerprint
