#include "fingerprint_matcher.h"

#include <algorithm>
#include <cmath>

namespace fingerprint {
namespace {

double cosineScore(const Features& enrolled, const Features& probe, std::size_t first, std::size_t end)
{
    double dot = 0.0;
    double enrolledNorm = 0.0;
    double probeNorm = 0.0;
    for (std::size_t i = first; i < end; ++i) {
        const double a = enrolled[i];
        const double b = probe[i];
        dot += a * b;
        enrolledNorm += a * a;
        probeNorm += b * b;
    }

    const double norm = std::sqrt(enrolledNorm * probeNorm);
    const double cosine = norm > 0.0 ? dot / norm : 0.0;
    return (std::clamp(cosine, -1.0, 1.0) + 1.0) / 2.0;
}

} // namespace

double compareFeatures(const Features& enrolled, const Features& probe)
{
    return fuseBranchScores(compareBranches(enrolled, probe), kTextureWeight);
}

double compareConcatenatedFeatures(const Features& enrolled, const Features& probe)
{
    return cosineScore(enrolled, probe, 0, kFeatureCount);
}

BranchSimilarities compareBranches(const Features& enrolled, const Features& probe)
{
    constexpr std::size_t branchSize = 256;
    static_assert(branchSize * 2 == kFeatureCount);
    return {cosineScore(enrolled, probe, 0, branchSize), cosineScore(enrolled, probe, branchSize, kFeatureCount)};
}

double fuseBranchScores(const BranchSimilarities& scores, double textureWeight)
{
    return textureWeight * scores.texture + (1.0 - textureWeight) * scores.minutiae;
}

} // namespace fingerprint
