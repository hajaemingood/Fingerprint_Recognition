#include "plan.h"
#include "fvc_dataset.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <stdexcept>

namespace evaluation {
namespace {

using Samples = std::vector<fvc::Sample>;
using Indexes = std::vector<std::size_t>;
using FingerGroups = std::map<std::pair<std::string, unsigned>, Indexes>;
using Seen = std::set<std::pair<std::size_t, std::size_t>>;

void selectImages(Indexes& images, const Samples& samples, Selection selection, std::mt19937& random)
{
    switch (selection.rule) {
    case Rule::All:
        break;
    case Rule::First:
        if (images.size() > 1) {
            images.resize(1);
        }
        break;
    case Rule::Impression:
        images.erase(std::remove_if(images.begin(), images.end(), [&](auto index) {
            return samples[index].impression != selection.value;
        }), images.end());
        break;
    case Rule::Random:
        std::shuffle(images.begin(), images.end(), random);
        if (images.size() > selection.value) {
            images.resize(selection.value);
        }
        break;
    }
}

void addTrial(std::vector<Trial>& trials, Seen& seen, const Samples& samples,
              std::size_t enrolled, std::size_t probe)
{
    if (enrolled == probe || samples[enrolled].database != samples[probe].database) {
        throw std::runtime_error("A pair must contain two different images from the same database");
    }
    if (seen.emplace(std::minmax(enrolled, probe)).second) {
        const auto& first = samples[enrolled];
        const auto& second = samples[probe];
        trials.push_back({first.database, first.relativePath, second.relativePath, first.finger == second.finger});
    }
}

std::vector<Trial> readPairs(const fs::path& path, const Samples& samples)
{
    std::map<std::string, std::size_t> index;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        index.emplace(samples[i].relativePath.generic_u8string(), i);
    }
    std::ifstream input(path);
    std::string line;
    if (!std::getline(input, line)) {
        throw std::runtime_error("Cannot read the pair CSV");
    }
    if (line.compare(0, 3, "\xef\xbb\xbf") == 0) {
        line.erase(0, 3);
    }
    if (line != "enrolled,probe" && line != "enrolled,probe\r") {
        throw std::runtime_error("Pair CSV must start with enrolled,probe");
    }
    std::vector<Trial> trials;
    Seen seen;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        const auto comma = line.find(',');
        if (comma == std::string::npos) {
            throw std::runtime_error("Invalid pair CSV row: " + line);
        }
        const auto first = index.find(fs::u8path(line.substr(0, comma)).generic_u8string());
        const auto second = index.find(fs::u8path(line.substr(comma + 1)).generic_u8string());
        if (first == index.end() || second == index.end()) {
            throw std::runtime_error("Pair CSV refers to an image outside the selected database: " + line);
        }
        addTrial(trials, seen, samples, first->second, second->second);
    }
    if (!input.eof()) {
        throw std::runtime_error("Failed to read the pair CSV");
    }
    return trials;
}

std::vector<Trial> selectPairs(const Samples& samples, const Settings& settings, std::mt19937& random)
{
    FingerGroups groups;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        groups[{samples[i].database, samples[i].finger}].push_back(i);
    }
    std::vector<Trial> trials;
    Seen seen;
    for (const auto& group : groups) {
        auto enrollment = group.second;
        selectImages(enrollment, samples, settings.enrollment, random);
        for (auto enrolled : enrollment) {
            Indexes genuine;
            for (auto probe : group.second) {
                if (probe != enrolled && !seen.count(std::minmax(enrolled, probe))) {
                    genuine.push_back(probe);
                }
            }
            selectImages(genuine, samples, settings.genuine, random);
            for (auto probe : genuine) {
                addTrial(trials, seen, samples, enrolled, probe);
            }
            for (const auto& other : groups) {
                if (other.first.first != group.first.first || other.first.second == group.first.second) {
                    continue;
                }
                auto impostor = other.second;
                if (settings.impostor.rule == Rule::Random) {
                    impostor.erase(std::remove_if(impostor.begin(), impostor.end(), [&](auto probe) {
                        return seen.count(std::minmax(enrolled, probe)) != 0;
                    }), impostor.end());
                }
                selectImages(impostor, samples, settings.impostor, random);
                for (auto probe : impostor) {
                    addTrial(trials, seen, samples, enrolled, probe);
                }
            }
        }
    }
    return trials;
}

} // namespace

std::vector<Trial> createPlan(const Settings& settings)
{
    const auto samples = fvc::scanDataset(settings.database, true);
    std::mt19937 random(settings.seed);
    auto trials = settings.pairFile.empty() ? selectPairs(samples, settings, random)
                                          : readPairs(settings.pairFile, samples);
    if (trials.empty()) {
        throw std::runtime_error("The selected rules produced no fingerprint pairs");
    }
    std::shuffle(trials.begin(), trials.end(), random);
    return trials;
}

} // namespace evaluation
