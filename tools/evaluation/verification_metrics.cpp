#include "verification_metrics.h"

#include <algorithm>

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
