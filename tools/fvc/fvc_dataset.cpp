#include "fvc_dataset.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <set>
#include <tuple>
#include <utility>

namespace fvc {
namespace {

namespace fs = std::filesystem;

std::string stripCarriageReturn(std::string line)
{
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    return line;
}

std::array<std::string_view, 8> fields(const std::string& line)
{
    std::array<std::string_view, 8> result;
    std::string_view remaining = line;
    for (std::size_t i = 0; i + 1 < result.size(); ++i) {
        const auto comma = remaining.find(',');
        if (comma == std::string_view::npos) {
            throw std::runtime_error("Incomplete noise manifest row");
        }
        result[i] = remaining.substr(0, comma);
        remaining.remove_prefix(comma + 1);
    }
    if (remaining.find(',') != std::string_view::npos) {
        throw std::runtime_error("Invalid noise manifest row");
    }
    result.back() = remaining;
    return result;
}

} // namespace

std::vector<Sample> scanDataset(const fs::path& root)
{
    std::vector<Sample> samples;
    std::set<std::tuple<std::string, unsigned int, unsigned int>> identifiers;
    for (const auto& directory : fs::directory_iterator(root)) {
        if (!directory.is_directory()) {
            continue;
        }
        const std::string database = directory.path().filename().u8string();
        if (database.find_first_not_of(
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") != std::string::npos) {
            throw std::runtime_error("Unsupported FVC directory name: " + database);
        }
        for (const auto& entry : fs::directory_iterator(directory.path())) {
            if (!entry.is_regular_file()) {
                continue;
            }
            const auto extension = entry.path().extension().wstring();
            if (_wcsicmp(extension.c_str(), L".tif") != 0 && _wcsicmp(extension.c_str(), L".tiff") != 0) {
                continue;
            }
            const auto stem = entry.path().stem().u8string();
            const auto underscore = stem.find('_');
            if (underscore == std::string::npos) {
                throw std::runtime_error("Expected a finger_impression TIFF: " + stem);
            }
            const auto finger = parseInteger<unsigned int>(std::string_view(stem).substr(0, underscore));
            const auto impression = parseInteger<unsigned int>(std::string_view(stem).substr(underscore + 1));
            if (finger == 0 || impression == 0 || !identifiers.emplace(database, finger, impression).second) {
                throw std::runtime_error("Invalid or duplicate FVC identifier: " + database + "/" + stem);
            }
            samples.push_back({database, finger, impression,
                               directory.path().filename() / entry.path().filename()});
        }
    }
    if (samples.empty()) {
        throw std::runtime_error("No FVC TIFFs found under " + root.u8string());
    }
    std::sort(samples.begin(), samples.end(), [](const Sample& first, const Sample& second) {
        return std::tie(first.database, first.finger, first.impression)
             < std::tie(second.database, second.finger, second.impression);
    });
    return samples;
}

fs::path probePath(const Sample& sample, int sigma)
{
    if (sigma == 0) {
        return sample.relativePath;
    }
    return sample.relativePath.parent_path() / ("sigma_" + std::to_string(sigma))
         / sample.relativePath.filename();
}

Manifest readManifest(const fs::path& path, const std::vector<Sample>& originals)
{
    std::ifstream file(path);
    std::string line;
    if (!std::getline(file, line) || stripCarriageReturn(line) != kManifestHeader) {
        throw std::runtime_error("Missing or invalid noise manifest: " + path.u8string());
    }

    std::map<std::string, std::size_t> positions;
    for (std::size_t i = 0; i < originals.size(); ++i) {
        positions.emplace(originals[i].relativePath.generic_u8string(), i);
    }

    Manifest manifest;
    while (std::getline(file, line)) {
        line = stripCarriageReturn(std::move(line));
        const auto row = fields(line);
        const auto found = positions.find(std::string(row[3]));
        if (found == positions.end()) {
            throw std::runtime_error("Unknown original in noise manifest: " + std::string(row[3]));
        }
        const auto index = found->second;
        const auto& sample = originals[index];
        const auto sigma = parseInteger<int>(row[5]);
        const auto baseSeed = parseInteger<std::uint64_t>(row[6]);
        const auto seed = parseInteger<std::uint64_t>(row[7]);
        const auto relative = probePath(sample, sigma);
        if (sigma < 0 || sigma > 255 || row[0] != sample.database
            || parseInteger<unsigned int>(row[1]) != sample.finger
            || parseInteger<unsigned int>(row[2]) != sample.impression
            || row[4] != relative.generic_u8string()) {
            throw std::runtime_error("Noise manifest row does not match its FVC sample");
        }
        if (manifest.bySigma.empty()) {
            manifest.baseSeed = baseSeed;
        } else if (baseSeed != manifest.baseSeed) {
            throw std::runtime_error("Noise manifest contains multiple base seeds");
        }
        auto& variant = manifest.bySigma.try_emplace(sigma, originals.size()).first->second[index];
        if (!variant.relativePath.empty()) {
            throw std::runtime_error("Duplicate FVC image and noise level in manifest");
        }
        variant = {relative, seed};
    }
    if (!file.eof() || !manifest.bySigma.count(0) || manifest.bySigma.size() < 2) {
        throw std::runtime_error("The manifest needs the baseline and at least one noise level");
    }
    for (const auto& level : manifest.bySigma) {
        for (const auto& variant : level.second) {
            if (variant.relativePath.empty()) {
                throw std::runtime_error("Incomplete FVC coverage at sigma " + std::to_string(level.first));
            }
        }
    }
    return manifest;
}

void createOutputDirectory(const fs::path& output)
{
    if (!output.parent_path().empty()) {
        fs::create_directories(output.parent_path());
    }
    if (!fs::create_directory(output)) {
        throw std::runtime_error("Choose a new output directory: " + output.u8string());
    }
}

} // namespace fvc
