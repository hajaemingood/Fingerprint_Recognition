#include "verification_metrics.h"

#include <algorithm>

namespace {

VerificationRates errorRates(VerificationCounts totals, VerificationCounts accepted)
{
    return {static_cast<double>(accepted.impostor) / totals.impostor,
            static_cast<double>(totals.genuine - accepted.genuine) / totals.genuine};
}

} // namespace

std::optional<double> equalErrorRate(std::vector<VerificationScore> scores)
{
    const auto genuine = static_cast<std::size_t>(std::count_if(
        scores.begin(), scores.end(), [](const VerificationScore& score) { return score.genuine; }));
    const auto impostors = scores.size() - genuine;
    if (genuine == 0 || impostors == 0) {
        return std::nullopt;
    }
    std::sort(scores.begin(), scores.end(), [](const auto& first, const auto& second) {
        return first.score > second.score;
    });

    std::size_t genuineAccepted = 0;
    std::size_t impostorsAccepted = 0;
    double previousFmr = 0.0;
    double previousDifference = -1.0;
    for (std::size_t i = 0; i < scores.size();) {
        const double threshold = scores[i].score;
        do {
            if (scores[i].genuine) {
                ++genuineAccepted;
            } else {
                ++impostorsAccepted;
            }
            ++i;
        } while (i < scores.size() && scores[i].score == threshold);

        const double fmr = static_cast<double>(impostorsAccepted) / impostors;
        const double fnmr = static_cast<double>(genuine - genuineAccepted) / genuine;
        const double difference = fmr - fnmr;
        if (difference >= 0.0) {
            const double fraction = -previousDifference / (difference - previousDifference);
            return previousFmr + fraction * (fmr - previousFmr);
        }
        previousFmr = fmr;
        previousDifference = difference;
    }
    return std::nullopt;
}

std::optional<VerificationRates> ratesAtThreshold(const std::vector<VerificationScore>& scores,
                                                   VerificationCounts totals, double threshold)
{
    if (totals.genuine == 0 || totals.impostor == 0) {
        return std::nullopt;
    }
    VerificationCounts accepted;
    for (const auto& result : scores) {
        if (result.score >= threshold) {
            ++(result.genuine ? accepted.genuine : accepted.impostor);
        }
    }
    return errorRates(totals, accepted);
}

std::optional<OperatingPoint> equalErrorOperatingPoint(std::vector<VerificationScore> scores,
                                                       VerificationCounts totals)
{
    if (totals.genuine == 0 || totals.impostor == 0) {
        return std::nullopt;
    }
    std::sort(scores.begin(), scores.end(), [](const auto& first, const auto& second) {
        return first.score > second.score;
    });

    VerificationCounts accepted;
    OperatingPoint previous{1.001, {0.0, 1.0}};
    for (std::size_t i = 0; i < scores.size();) {
        const double threshold = scores[i].score;
        do {
            ++(scores[i].genuine ? accepted.genuine : accepted.impostor);
            ++i;
        } while (i < scores.size() && scores[i].score == threshold);

        const OperatingPoint current{threshold, errorRates(totals, accepted)};
        if (current.rates.fmr == current.rates.fnmr) {
            return current;
        }
        if (current.rates.fmr > current.rates.fnmr) {
            return current.averageError() < previous.averageError() ? current : previous;
        }
        previous = current;
    }
    return std::nullopt;
}
