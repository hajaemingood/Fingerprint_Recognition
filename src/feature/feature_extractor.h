#pragma once

#include "fingerprint_features.h"

namespace fingerprint {

// Returns false for an unsupported iType. The caller validates image.
bool extractFeatures(int iType, const unsigned char* image, Features& features);

} // namespace fingerprint
