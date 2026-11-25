#pragma once

#include "fingerprint_features.h"

namespace fingerprint {

// Returns false for an unsupported iType. Model and inference failures throw.
bool extractFeatures(int iType, const unsigned char* image, Features& features);

} // namespace fingerprint
