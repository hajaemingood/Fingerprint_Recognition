#pragma once

#include <cstddef>
#include <optional>
#include <vector>

struct VerificationScore {
    double score;
    bool genuine;
};

// Interpolate between the neighboring operating points that bound FMR = FNMR.
// A score equal to the threshold is accepted. Ties move as one group.
std::optional<double> equalErrorRate(std::vector<VerificationScore> scores);

struct VerificationCounts {
    std::size_t genuine = 0;
    std::size_t impostor = 0;
};

struct VerificationRates {
    double fmr;
    double fnmr;
};

struct OperatingPoint {
    double threshold;
    VerificationRates rates;

    double averageError() const { return (rates.fmr + rates.fnmr) / 2.0; }
};

// totals includes every planned pair. scores contains successful comparisons.
std::optional<VerificationRates> ratesAtThreshold(const std::vector<VerificationScore>& scores,
                                                   VerificationCounts totals, double threshold);
// Choose the exact crossing or the neighboring point with the smaller average error.
std::optional<OperatingPoint> equalErrorOperatingPoint(std::vector<VerificationScore> scores,
                                                       VerificationCounts totals);
