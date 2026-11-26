#pragma once

#include "feature/fingerprint_features.h"

namespace fingerprint {

struct BranchSimilarities {
    double texture;
    double minutiae;
};

#ifdef FINGERPRINT_TEXTURE_WEIGHT
inline constexpr double kTextureWeight = FINGERPRINT_TEXTURE_WEIGHT;
#else
inline constexpr double kTextureWeight = 0.22;
#endif
static_assert(kTextureWeight >= 0.0 && kTextureWeight <= 1.0, "Texture weight must be in [0, 1]");

double compareFeatures(const Features& enrolled, const Features& probe);
double compareConcatenatedFeatures(const Features& enrolled, const Features& probe);
BranchSimilarities compareBranches(const Features& enrolled, const Features& probe);
double fuseBranchScores(const BranchSimilarities& scores, double textureWeight);

} // namespace fingerprint
