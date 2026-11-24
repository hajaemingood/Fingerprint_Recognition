#include "fvc_dataset.h"
#include "kisa_library.h"
#include "tiff_image.h"
#include "verification_metrics.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <utility>

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
constexpr std::size_t kTemplateCapacity = 1000000;

struct TemplateResult {
    std::vector<unsigned char> bytes;
    int error = 0;
    std::string message;
};

struct ImageStats {
    std::size_t failures = 0;
    double milliseconds = 0.0;
    std::size_t templateBytes = 0;
};

double elapsedMilliseconds(Clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

std::string messageText(const std::array<char, 256>& buffer)
{
    return {buffer.begin(), std::find(buffer.begin(), buffer.end(), '\0')};
}

TemplateResult extract(decltype(&KISA_Enroll) function, fvc::GrayImage& image,
                       std::vector<unsigned char>& buffer, ImageStats& stats)
{
    TemplateResult result;
    int size = 0;
    std::array<char, 256> message{};
    const auto start = Clock::now();
    const int status = function(image.type, image.pixels.data(), buffer.data(), &size,
                                &result.error, message.data());
    stats.milliseconds += elapsedMilliseconds(start);
    if (status == 1 && size > 0 && static_cast<std::size_t>(size) < buffer.size()) {
        result.bytes.assign(buffer.begin(), buffer.begin() + size);
        stats.templateBytes += static_cast<std::size_t>(size);
    } else {
        ++stats.failures;
        result.message = status == 1 ? "Invalid template size" : messageText(message);
    }
    return result;
}

void writeCsvString(std::ostream& out, const std::string& text)
{
    out << '"';
    for (const char character : text) {
        if (character == '"') {
            out << '"';
        }
        out << character;
    }
    out << '"';
}

struct EvaluationStats {
    std::size_t genuinePairs = 0;
    std::size_t impostorPairs = 0;
    std::size_t genuineScored = 0;
    std::size_t impostorScored = 0;
    std::size_t comparisons = 0;
    std::size_t comparisonFailures = 0;
    double comparisonMilliseconds = 0.0;
    std::vector<VerificationScore> scores;
};

void evaluatePair(const KisaLibrary& dll, TemplateResult& enrolled, TemplateResult& probe,
                  bool genuine, EvaluationStats& stats, std::ostream& row)
{
    ++(genuine ? stats.genuinePairs : stats.impostorPairs);
    if (enrolled.bytes.empty() || probe.bytes.empty()) {
        const bool enrollmentFailed = enrolled.bytes.empty();
        const auto& failure = enrollmentFailed ? enrolled : probe;
        row << (enrollmentFailed ? "enroll_failed" : "attempt_failed")
            << ",," << failure.error << ',';
        writeCsvString(row, failure.message);
        return;
    }

    double score = 0.0;
    int error = 0;
    std::array<char, 256> message{};
    ++stats.comparisons;
    const auto start = Clock::now();
    const int status = dll.compare(enrolled.bytes.data(), probe.bytes.data(), &score,
                                   &error, message.data());
    stats.comparisonMilliseconds += elapsedMilliseconds(start);
    if (status != 1 || !std::isfinite(score) || score < 0.0 || score > 1.0) {
        ++stats.comparisonFailures;
        row << "compare_failed,," << error << ',';
        writeCsvString(row, status == 1 ? "Invalid comparison score" : messageText(message));
        return;
    }

    stats.scores.push_back({score, genuine});
    ++(genuine ? stats.genuineScored : stats.impostorScored);
    row << "ok," << score << ",0,";
}

void writeMean(std::ostream& out, double total, std::size_t count)
{
    if (count == 0) {
        out << "NA";
    } else {
        out << total / count;
    }
}

void writeSigma(std::ostream& out, const fvc::Condition& condition)
{
    if (condition.kind == fvc::TransformKind::Original || condition.kind == fvc::TransformKind::Gaussian) {
        out << condition.parameter;
    }
}

bool evaluateDatabase(const KisaLibrary& dll, const fs::path& originalRoot, const fs::path& augmentationRoot,
                      const std::vector<fvc::Sample>& samples, const fvc::Manifest& manifest,
                      std::size_t first, std::size_t last, std::ostream& pairs, std::ostream& summary)
{
    const std::size_t count = last - first;
    const auto& database = samples[first].database;
    std::vector<unsigned char> buffer(kTemplateCapacity);
    std::vector<TemplateResult> enrolled;
    ImageStats enrollmentStats;
    int imageType = 0;
    for (std::size_t i = first; i < last; ++i) {
        auto image = fvc::readTiff(originalRoot / samples[i].relativePath);
        if (imageType != 0 && image.type != imageType) {
            throw std::runtime_error("Mixed image sizes within " + database);
        }
        imageType = image.type;
        enrolled.push_back(extract(dll.enroll, image, buffer, enrollmentStats));
    }

    bool complete = true;
    std::optional<double> referenceThreshold;
    for (const auto& level : manifest.conditions) {
        const auto& condition = level.first;
        const auto& variants = level.second;
        const bool original = condition.kind == fvc::TransformKind::Original;
        std::vector<TemplateResult> probes;
        ImageStats attemptStats;
        for (std::size_t i = first; i < last; ++i) {
            auto image = fvc::readTiff((original ? originalRoot : augmentationRoot) / variants[i].relativePath);
            if (image.type != imageType) {
                throw std::runtime_error("Probe image size differs from its FVC database");
            }
            probes.push_back(extract(dll.attempt, image, buffer, attemptStats));
        }

        EvaluationStats stats;
        for (std::size_t i = first; i < last; ++i) {
            for (std::size_t j = i + 1; j < last; ++j) {
                const bool genuine = samples[i].finger == samples[j].finger;
                pairs << database << ',' << samples[i].relativePath.generic_u8string()
                      << ',' << variants[j].relativePath.generic_u8string() << ',' << genuine << ',';
                writeSigma(pairs, condition);
                pairs << ',' << condition.baseSeed << ',' << variants[j].seed << ',';
                evaluatePair(dll, enrolled[i - first], probes[j - first], genuine, stats, pairs);
                if (manifest.robustness) {
                    pairs << ',' << fvc::transformName(condition.kind) << ',' << condition.parameter
                          << ',' << samples[j].relativePath.generic_u8string();
                }
                pairs << '\n';
            }
        }

        std::optional<double> eer;
        std::optional<VerificationRates> referenceRates;
        if (manifest.robustness) {
            const VerificationCounts totals{stats.genuinePairs, stats.impostorPairs};
            const auto point = equalErrorOperatingPoint(stats.scores, totals);
            if (point) {
                eer = point->averageError();
                if (original) {
                    referenceThreshold = point->threshold;
                }
            }
            if (referenceThreshold) {
                referenceRates = ratesAtThreshold(stats.scores, totals, *referenceThreshold);
            }
        } else {
            eer = equalErrorRate(std::move(stats.scores));
        }
        summary << database << ',';
        writeSigma(summary, condition);
        summary << ',' << condition.baseSeed << ',' << count
                << ',' << stats.genuinePairs << ',' << stats.impostorPairs
                << ',' << stats.genuineScored << ',' << stats.impostorScored
                << ',' << enrollmentStats.failures << ',' << attemptStats.failures
                << ',' << stats.comparisonFailures << ',';
        if (eer) {
            summary << *eer;
        } else {
            summary << "NA";
            complete = false;
        }
        summary << ',' << static_cast<double>(enrollmentStats.failures) / count
                << ',' << static_cast<double>(attemptStats.failures) / count << ',';
        writeMean(summary, enrollmentStats.milliseconds, count);
        summary << ',';
        writeMean(summary, attemptStats.milliseconds, count);
        summary << ',';
        writeMean(summary, stats.comparisonMilliseconds, stats.comparisons);
        summary << ',';
        writeMean(summary, static_cast<double>(enrollmentStats.templateBytes),
                  count - enrollmentStats.failures);
        summary << ',';
        writeMean(summary, static_cast<double>(attemptStats.templateBytes), count - attemptStats.failures);
        if (manifest.robustness) {
            summary << ',' << fvc::transformName(condition.kind) << ',' << condition.parameter << ',';
            if (referenceRates) {
                summary << *referenceThreshold << ',' << referenceRates->fmr << ',' << referenceRates->fnmr;
            } else {
                summary << "NA,NA,NA";
            }
        }
        summary << '\n';
        std::cout << database << ' ';
        if (manifest.robustness) {
            std::cout << fvc::transformName(condition.kind) << ' ' << condition.parameter;
            if (condition.kind == fvc::TransformKind::Gaussian) {
                std::cout << " seed " << condition.baseSeed;
            }
        } else {
            std::cout << "sigma " << condition.parameter;
        }
        std::cout << ": " << stats.genuineScored + stats.impostorScored << " scored pairs";
        if (eer) {
            std::cout << ", EER " << 100.0 * *eer << '%';
        } else {
            std::cout << ", EER unavailable (see pair statuses)";
        }
        std::cout << '\n' << std::flush;
    }
    return complete;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc != 5) {
        std::cerr << "Usage: evaluate_noise <fvc-root> <augmentation-dir> <kisa-dll> <new-results-dir>\n";
        return 2;
    }

    try {
        const fs::path originals = argv[1];
        const fs::path augmentation = argv[2];
        const fs::path results = argv[4];
        const auto samples = fvc::scanDataset(originals);
        const auto manifest = fvc::readManifest(augmentation / "manifest.csv", samples);
        const KisaLibrary dll(argv[3]);
        fvc::createOutputDirectory(results);
        std::ofstream pairs(results / "pairs.csv.tmp");
        std::ofstream summary(results / "summary.csv.tmp");
        pairs.exceptions(std::ios::failbit | std::ios::badbit);
        summary.exceptions(std::ios::failbit | std::ios::badbit);
        pairs << "database,enrolled,probe,genuine,sigma,base_seed,seed,status,score,error_code,error_message";
        summary << "database,sigma,base_seed,images,genuine_pairs,impostor_pairs,genuine_scored,impostor_scored,"
                   "enroll_failed,attempt_failed,compare_failed,eer,fte,fta,mean_enroll_ms,mean_attempt_ms,"
                   "mean_compare_ms,mean_enroll_bytes,mean_attempt_bytes";
        if (manifest.robustness) {
            pairs << ",transform,parameter,probe_original";
            summary << ",transform,parameter,reference_threshold,fmr_at_reference,fnmr_at_reference";
        }
        pairs << '\n' << std::setprecision(9);
        summary << '\n' << std::setprecision(9);

        bool complete = true;
        for (std::size_t first = 0; first < samples.size();) {
            std::size_t last = first + 1;
            while (last < samples.size() && samples[last].database == samples[first].database) {
                ++last;
            }
            complete = evaluateDatabase(dll, originals, augmentation, samples, manifest, first, last, pairs, summary)
                       && complete;
            first = last;
        }
        pairs.close();
        summary.close();
        fs::rename(results / "pairs.csv.tmp", results / "pairs.csv");
        fs::rename(results / "summary.csv.tmp", results / "summary.csv");
        return complete ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "evaluate_noise: " << error.what() << '\n';
        return 1;
    }
}
