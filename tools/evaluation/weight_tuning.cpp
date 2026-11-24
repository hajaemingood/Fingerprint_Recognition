#include "feature/template_manager.h"
#include "fvc_dataset.h"
#include "matching/fingerprint_matcher.h"
#include "verification_metrics.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <optional>
#include <random>
#include <set>

namespace {

namespace fs = std::filesystem;
using fingerprint::BranchSimilarities;
using fingerprint::Features;
constexpr unsigned splitSeed = 42;
constexpr int weightSteps = 100;
constexpr double tolerance = 1e-12;

enum class Split { Development, Confirmation, CrossSplit, All };
using Identity = std::pair<std::string, unsigned>;
using Assignments = std::map<Identity, Split>;

const char* splitName(Split split)
{
    switch (split) {
    case Split::Development: return "development";
    case Split::Confirmation: return "confirmation";
    case Split::CrossSplit: return "cross_split";
    case Split::All: return "all";
    }
    throw std::logic_error("Invalid split");
}

Assignments splitFingers(const std::vector<fvc::Sample>& samples)
{
    std::map<std::string, std::set<unsigned>> fingers;
    for (const auto& sample : samples) {
        fingers[sample.database].insert(sample.finger);
    }

    std::mt19937 random(splitSeed);
    Assignments assignments;
    for (const auto& entry : fingers) {
        std::vector<unsigned> ids(entry.second.begin(), entry.second.end());
        if (ids.size() < 4) {
            throw std::runtime_error("Each database needs at least four fingers: " + entry.first);
        }
        std::shuffle(ids.begin(), ids.end(), random);
        for (std::size_t i = 0; i < ids.size(); ++i) {
            assignments.emplace(Identity{entry.first, ids[i]},
                                i < ids.size() / 2 ? Split::Development : Split::Confirmation);
        }
    }
    return assignments;
}

class TemplateCache {
public:
    explicit TemplateCache(fs::path root) : root_(std::move(root)) {}

    const Features& read(const char* role, const fs::path& relative)
    {
        auto path = root_ / role / relative;
        path += ".bin";
        const auto found = cache_.find(path);
        if (found != cache_.end()) {
            return found->second;
        }

        std::ifstream file(path, std::ios::binary);
        std::array<unsigned char, fingerprint::kTemplateSize> bytes{};
        file.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
        Features features{};
        if (!file || file.peek() != std::char_traits<char>::eof()
            || !fingerprint::decodeTemplate(bytes.data(), features)) {
            throw std::runtime_error("Invalid or missing DeepPrint template: " + path.u8string());
        }
        return cache_.emplace(std::move(path), features).first->second;
    }

private:
    fs::path root_;
    std::map<fs::path, Features> cache_;
};

struct PairScores {
    BranchSimilarities branches;
    double baseline;
    bool genuine;
    Split split;
};
using DatabasePairs = std::map<std::string, std::vector<PairScores>>;

struct ScoredRun {
    DatabasePairs pairs;
    const char* recordedScorer;
};

double quantize(double score)
{
    return std::round(score * 1000.0) / 1000.0;
}

std::string_view readField(std::string_view& line)
{
    const auto comma = line.find(',');
    if (comma == std::string_view::npos) {
        throw std::runtime_error("Incomplete evaluator pairs CSV row");
    }
    const auto field = line.substr(0, comma);
    line.remove_prefix(comma + 1);
    return field;
}

ScoredRun loadPairs(const fs::path& path, const std::vector<fvc::Sample>& samples,
                    const Assignments& assignments, TemplateCache& templates)
{
    std::map<std::string, const fvc::Sample*> index;
    std::map<std::string, std::size_t> imageCounts;
    for (const auto& sample : samples) {
        index.emplace(sample.relativePath.generic_u8string(), &sample);
        ++imageCounts[sample.database];
    }

    std::ifstream csv(path);
    std::string line;
    constexpr std::string_view header = "sequence,database,enrolled,probe,genuine,status,stage,score,";
    if (!std::getline(csv, line) || line.compare(0, header.size(), header) != 0) {
        throw std::runtime_error("Expected the evaluation app's pairs.csv");
    }

    DatabasePairs pairs;
    std::set<std::pair<std::string, std::string>> seen;
    bool matchesBaseline = true;
    bool matchesCurrent = true;
    while (std::getline(csv, line)) {
        std::string_view fields(line);
        readField(fields); // Sequence does not affect tuning.
        const std::string database(readField(fields));
        const std::string enrollment(readField(fields));
        const std::string attempt(readField(fields));
        const auto genuine = readField(fields);
        if (readField(fields) != "ok" || readField(fields) != "compare") {
            throw std::runtime_error("Use a completed original-image run with scores for all pairs");
        }
        const std::string scoreField(readField(fields));
        std::size_t consumed = 0;
        const double recorded = std::stod(scoreField, &consumed);
        if (consumed != scoreField.size() || !std::isfinite(recorded) || recorded < 0.0 || recorded > 1.0) {
            throw std::runtime_error("Invalid score in pairs.csv");
        }

        const auto first = index.find(enrollment);
        const auto second = index.find(attempt);
        if (first == index.end() || second == index.end() || enrollment == attempt
            || first->second->database != database || second->second->database != database
            || !seen.emplace(std::minmax(enrollment, attempt)).second) {
            throw std::runtime_error("Invalid or duplicate original-image pair: " + enrollment + ", " + attempt);
        }
        const auto& enrolled = *first->second;
        const auto& probe = *second->second;
        const bool sameFinger = enrolled.finger == probe.finger;
        if (genuine != (sameFinger ? "1" : "0")) {
            throw std::runtime_error("Pair label does not match the FVC finger IDs");
        }
        const auto enrolledSplit = assignments.at({database, enrolled.finger});
        const auto probeSplit = assignments.at({database, probe.finger});
        const auto split = enrolledSplit == probeSplit ? enrolledSplit : Split::CrossSplit;
        const auto& a = templates.read("enroll", enrolled.relativePath);
        const auto& b = templates.read("attempt", probe.relativePath);
        const double baseline = quantize(fingerprint::compareConcatenatedFeatures(a, b));
        const auto branches = fingerprint::compareBranches(a, b);
        const double current = quantize(fingerprint::fuseBranchScores(branches, fingerprint::kTextureWeight));
        matchesBaseline = matchesBaseline && std::abs(recorded - baseline) <= 1e-9;
        matchesCurrent = matchesCurrent && std::abs(recorded - current) <= 1e-9;
        if (!matchesBaseline && !matchesCurrent) {
            throw std::runtime_error("Cached templates do not reproduce the run score for " + enrollment + ", " + attempt);
        }
        pairs[database].push_back({branches, baseline, sameFinger, split});
    }
    if (!csv.eof()) {
        throw std::runtime_error("Could not finish reading " + path.u8string());
    }
    for (const auto& entry : imageCounts) {
        if (pairs[entry.first].size() != entry.second * (entry.second - 1) / 2) {
            throw std::runtime_error("The evaluation pair list is incomplete for " + entry.first);
        }
    }
    return {std::move(pairs), matchesBaseline ? "concatenated_cosine" : "current_weighted"};
}

struct ScoreGroup {
    std::string database;
    std::vector<VerificationScore> scores;
    VerificationCounts counts;
};

double thresholdAtOnePercent(const ScoreGroup& group)
{
    std::vector<double> impostors;
    for (const auto& score : group.scores) {
        if (!score.genuine) {
            impostors.push_back(score.score);
        }
    }
    std::sort(impostors.begin(), impostors.end(), std::greater<double>());
    const auto allowed = impostors.size() / 100;
    return (std::round(impostors.at(allowed) * 1000.0) + 1.0) / 1000.0;
}

struct DatabaseMetrics {
    std::string database;
    VerificationCounts counts;
    double eer;
    VerificationRates operatingRates;
};

struct Metrics {
    std::vector<DatabaseMetrics> databases;
    double threshold = 0.0;
    double meanEer = 0.0;
};

// A missing weight means the original cosine of the concatenated vector.
Metrics evaluate(const DatabasePairs& pairs, Split split, std::optional<double> weight,
                 std::optional<double> calibratedThreshold = std::nullopt)
{
    std::vector<ScoreGroup> groups;
    Metrics metrics;
    metrics.threshold = calibratedThreshold.value_or(0.0);
    for (const auto& entry : pairs) {
        ScoreGroup group{entry.first, {}, {}};
        for (const auto& pair : entry.second) {
            if (split != Split::All && pair.split != split) {
                continue;
            }
            const double score = weight ? quantize(fingerprint::fuseBranchScores(pair.branches, *weight))
                                        : pair.baseline;
            group.scores.push_back({score, pair.genuine});
            ++(pair.genuine ? group.counts.genuine : group.counts.impostor);
        }
        if (group.counts.genuine == 0 || group.counts.impostor == 0) {
            throw std::runtime_error("Each split needs genuine and impostor pairs in " + entry.first);
        }
        if (!calibratedThreshold) {
            metrics.threshold = std::max(metrics.threshold, thresholdAtOnePercent(group));
        }
        groups.push_back(std::move(group));
    }

    for (const auto& group : groups) {
        const auto point = equalErrorOperatingPoint(group.scores, group.counts);
        const auto operating = ratesAtThreshold(group.scores, group.counts, metrics.threshold);
        metrics.databases.push_back({group.database, group.counts, point.value().averageError(), operating.value()});
        metrics.meanEer += metrics.databases.back().eer;
    }
    metrics.meanEer /= metrics.databases.size();
    return metrics;
}

std::ofstream openOutput(const fs::path& path)
{
    std::ofstream out(path);
    out.exceptions(std::ios::failbit | std::ios::badbit);
    out << std::fixed << std::setprecision(8);
    return out;
}

void writeMetrics(std::ostream& out, const char* scorer, std::optional<double> weight,
                  Split split, const Metrics& metrics)
{
    for (const auto& db : metrics.databases) {
        out << scorer << ',';
        if (weight) {
            out << *weight;
        }
        out << ',' << splitName(split) << ',' << db.database << ',' << db.counts.genuine << ','
            << db.counts.impostor << ',' << db.eer << ',' << metrics.meanEer << ','
            << metrics.threshold << ',' << db.operatingRates.fmr << ',' << db.operatingRates.fnmr << '\n';
    }
}

constexpr char metricsHeader[] =
    "scorer,texture_weight,split,database,genuine_pairs,impostor_pairs,eer,macro_eer,"
    "development_threshold,fmr_at_threshold,fnmr_at_threshold\n";

void runTuning(const fs::path& fvcRoot, const fs::path& pairsPath, const fs::path& templateRoot,
               const fs::path& output)
{
    const auto samples = fvc::scanDataset(fvcRoot, true);
    const auto assignments = splitFingers(samples);
    TemplateCache templates(templateRoot);
    const auto run = loadPairs(pairsPath, samples, assignments, templates);
    const auto& pairs = run.pairs;

    const auto developmentBaseline = evaluate(pairs, Split::Development, std::nullopt);
    int selected = weightSteps / 2;
    std::vector<Metrics> sweep;
    for (int i = 0; i <= weightSteps; ++i) {
        sweep.push_back(evaluate(pairs, Split::Development, static_cast<double>(i) / weightSteps));
        if (i == 0) {
            selected = 0;
            continue;
        }
        const double difference = sweep[i].meanEer - sweep[selected].meanEer;
        if (difference < -tolerance ||
            (std::abs(difference) <= tolerance &&
             std::abs(i - weightSteps / 2) < std::abs(selected - weightSteps / 2))) {
            selected = i;
        }
    }
    const double weight = static_cast<double>(selected) / weightSteps;
    const auto& developmentSelected = sweep[selected];
    const auto confirmationBaseline = evaluate(pairs, Split::Confirmation, std::nullopt,
                                               developmentBaseline.threshold);
    const auto confirmationSelected = evaluate(pairs, Split::Confirmation, weight, developmentSelected.threshold);
    const auto fullBaseline = evaluate(pairs, Split::All, std::nullopt, developmentBaseline.threshold);
    const auto fullSelected = evaluate(pairs, Split::All, weight, developmentSelected.threshold);

    fvc::createOutputDirectory(output);
    auto assignmentsFile = openOutput(output / "split.csv");
    assignmentsFile << "database,finger,split\n";
    for (const auto& entry : assignments) {
        assignmentsFile << entry.first.first << ',' << entry.first.second << ',' << splitName(entry.second) << '\n';
    }
    assignmentsFile.close();

    auto development = openOutput(output / "development.csv");
    development << metricsHeader;
    writeMetrics(development, "baseline", std::nullopt, Split::Development, developmentBaseline);
    for (int i = 0; i <= weightSteps; ++i) {
        writeMetrics(development, "weighted", static_cast<double>(i) / weightSteps, Split::Development, sweep[i]);
    }
    development.close();

    auto comparison = openOutput(output / "comparison.csv");
    comparison << metricsHeader;
    writeMetrics(comparison, "baseline", std::nullopt, Split::Confirmation, confirmationBaseline);
    writeMetrics(comparison, "selected", weight, Split::Confirmation, confirmationSelected);
    writeMetrics(comparison, "baseline", std::nullopt, Split::All, fullBaseline);
    writeMetrics(comparison, "selected", weight, Split::All, fullSelected);
    comparison.close();

    auto summary = openOutput(output / "summary.txt");
    summary << "fvc_root=" << fs::absolute(fvcRoot).u8string() << '\n'
            << "pairs_file=" << fs::absolute(pairsPath).u8string() << '\n'
            << "template_directory=" << fs::absolute(templateRoot).u8string() << '\n'
            << "scope=original FVC images\n"
            << "split_seed=" << splitSeed << '\n'
            << "weights_tested=" << sweep.size() << '\n'
            << "selection=lowest development macro EER; tie closest to 0.50\n"
            << "operating_threshold=least threshold achieving development FMR <= 1% in every database\n"
            << "rate_units=fractions\n"
            << "selected_texture_weight=" << weight << '\n'
            << "selected_minutiae_weight=" << 1.0 - weight << '\n'
            << "development_baseline_macro_eer=" << developmentBaseline.meanEer << '\n'
            << "development_selected_macro_eer=" << developmentSelected.meanEer << '\n'
            << "confirmation_baseline_macro_eer=" << confirmationBaseline.meanEer << '\n'
            << "confirmation_selected_macro_eer=" << confirmationSelected.meanEer << '\n'
            << "all_baseline_macro_eer=" << fullBaseline.meanEer << '\n'
            << "all_selected_macro_eer=" << fullSelected.meanEer << '\n'
            << "recorded_scores_verified=yes\n"
            << "recorded_scorer=" << run.recordedScorer << '\n';
    summary.close();

    std::cout << std::fixed << std::setprecision(2) << "Selected texture weight " << weight
              << " and minutiae weight " << 1.0 - weight
              << "\nConfirmation mean EER: " << 100.0 * confirmationBaseline.meanEer << "% -> "
              << 100.0 * confirmationSelected.meanEer << "%\nLogs: " << output.u8string() << '\n';
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc != 5) {
        std::cerr << "Usage: tune_weights <fvc-root> <pairs.csv> <worker-template-directory> <new-log-directory>\n";
        return 2;
    }
    try {
        runTuning(argv[1], argv[2], argv[3], argv[4]);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "tune_weights: " << error.what() << '\n';
        return 1;
    }
}
