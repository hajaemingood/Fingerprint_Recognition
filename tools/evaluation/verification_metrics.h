#pragma once

#include <optional>
#include <vector>

struct VerificationScore {
    double score;
    bool genuine;
};

// Interpolate between the neighboring operating points that bound FMR = FNMR.
// A score equal to the threshold is accepted. Ties move as one group.
std::optional<double> equalErrorRate(std::vector<VerificationScore> scores);
