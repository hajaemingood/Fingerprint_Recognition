#pragma once

#include "image_format.h"

#include <vector>

namespace fingerprint {

inline constexpr int kDeepPrintInputSize = 299;

// Optional foreground crop, white square padding, and antialiased resize to [0, 1].
std::vector<float> preprocessDeepPrint(const unsigned char* image, ImageFormat format, bool cropForeground = false);

} // namespace fingerprint
