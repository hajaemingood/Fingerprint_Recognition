#pragma once

#include "feature/fingerprint_features.h"

namespace fingerprint {

double compareFeatures(const Features& enrolled, const Features& probe);

} // namespace fingerprint
