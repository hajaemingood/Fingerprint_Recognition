#pragma once

#include "image_format.h"

#include <vector>

namespace fingerprint {

inline constexpr int kDeepPrintInputSize = 299;

// White square padding followed by antialiased bilinear resize, in [0, 1].
std::vector<float> preprocessDeepPrint(const unsigned char* image, ImageFormat format);

} // namespace fingerprint
