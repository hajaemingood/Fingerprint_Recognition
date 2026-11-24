#pragma once

#include "feature/fingerprint_features.h"

namespace fingerprint {

struct BranchSimilarities {
    double texture;
    double minutiae;
};

inline constexpr double kTextureWeight = 0.22;

double compareFeatures(const Features& enrolled, const Features& probe);
double compareConcatenatedFeatures(const Features& enrolled, const Features& probe);
BranchSimilarities compareBranches(const Features& enrolled, const Features& probe);
double fuseBranchScores(const BranchSimilarities& scores, double textureWeight);

} // namespace fingerprint
