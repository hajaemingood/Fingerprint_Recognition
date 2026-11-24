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

template <std::size_t Count>
std::array<std::string_view, Count> fields(const std::string& line)
{
    std::array<std::string_view, Count> result;
    std::string_view remaining = line;
    for (std::size_t i = 0; i + 1 < result.size(); ++i) {
        const auto comma = remaining.find(',');
        if (comma == std::string_view::npos) {
            throw std::runtime_error("Incomplete augmentation manifest row");
        }
        result[i] = remaining.substr(0, comma);
        remaining.remove_prefix(comma + 1);
    }
    if (remaining.find(',') != std::string_view::npos) {
        throw std::runtime_error("Invalid augmentation manifest row");
    }
    result.back() = remaining;
    return result;
}

TransformKind parseTransform(std::string_view name)
{
    for (const auto kind : {TransformKind::Original, TransformKind::Gaussian, TransformKind::Rotation,
                            TransformKind::Brightness, TransformKind::Flip}) {
        if (transformName(kind) == name) {
            return kind;
        }
    }
    throw std::runtime_error("Unknown augmentation transform: " + std::string(name));
}

bool validParameter(const Condition& condition)
{
    const int value = condition.parameter;
    switch (condition.kind) {
    case TransformKind::Original: return value == 0;
    case TransformKind::Gaussian: return value > 0 && value <= 255;
    case TransformKind::Rotation: return value != 0 && value >= -180 && value <= 180;
    case TransformKind::Brightness: return value != 0 && value >= -255 && value <= 255;
    case TransformKind::Flip: return value == 0;
    }
    return false;
}

bool isTiff(const fs::directory_entry& entry)
{
    const auto extension = entry.path().extension().wstring();
    return entry.is_regular_file()
        && (_wcsicmp(extension.c_str(), L".tif") == 0 || _wcsicmp(extension.c_str(), L".tiff") == 0);
}

} // namespace

std::vector<Sample> scanDataset(const fs::path& root, bool allowSingleDatabase)
{
    std::vector<Sample> samples;
    std::set<std::tuple<std::string, unsigned int, unsigned int>> identifiers;
    const bool single = allowSingleDatabase
        && std::any_of(fs::directory_iterator(root), fs::directory_iterator(), isTiff);
    std::vector<fs::path> directories;
    if (single) {
        directories.push_back(root);
    } else {
        for (const auto& directory : fs::directory_iterator(root)) {
            if (directory.is_directory()) {
                directories.push_back(directory.path());
            }
        }
    }
    for (const auto& directory : directories) {
        const std::string database = directory.filename().u8string();
        if (database.find_first_not_of(
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") != std::string::npos) {
            throw std::runtime_error("Unsupported FVC directory name: " + database);
        }
        for (const auto& entry : fs::directory_iterator(directory)) {
            if (!isTiff(entry)) {
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
            const auto relative = single ? entry.path().filename() : directory.filename() / entry.path().filename();
            samples.push_back({database, finger, impression, relative});
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

bool Condition::operator<(const Condition& other) const
{
    return std::tie(kind, parameter, baseSeed) < std::tie(other.kind, other.parameter, other.baseSeed);
}

std::string_view transformName(TransformKind kind)
{
    switch (kind) {
    case TransformKind::Original: return "original";
    case TransformKind::Gaussian: return "gaussian";
    case TransformKind::Rotation: return "rotation";
    case TransformKind::Brightness: return "brightness";
    case TransformKind::Flip: return "flip";
    }
    throw std::runtime_error("Unknown augmentation transform");
}

fs::path probePath(const Sample& sample, const Condition& condition, bool robustness)
{
    if (condition.kind == TransformKind::Original) {
        return sample.relativePath;
    }

    std::string folder = condition.kind == TransformKind::Gaussian
        ? "sigma" : std::string(transformName(condition.kind));
    if (condition.kind != TransformKind::Flip) {
        folder += "_" + std::to_string(condition.parameter);
    }
    if (robustness && condition.kind == TransformKind::Gaussian) {
        folder += "_seed_" + std::to_string(condition.baseSeed);
    }
    return sample.relativePath.parent_path() / folder / sample.relativePath.filename();
}

Manifest readManifest(const fs::path& path, const std::vector<Sample>& originals)
{
    std::ifstream file(path);
    std::string line;
    if (!std::getline(file, line)) {
        throw std::runtime_error("Missing or empty augmentation manifest: " + path.u8string());
    }
    const auto header = stripCarriageReturn(std::move(line));
    Manifest manifest;
    manifest.robustness = header == kRobustnessManifestHeader;
    if (!manifest.robustness && header != kManifestHeader) {
        throw std::runtime_error("Invalid augmentation manifest header: " + path.u8string());
    }

    std::map<std::string, std::size_t> positions;
    for (std::size_t i = 0; i < originals.size(); ++i) {
        positions.emplace(originals[i].relativePath.generic_u8string(), i);
    }

    std::uint64_t legacySeed = 0;
    while (std::getline(file, line)) {
        line = stripCarriageReturn(std::move(line));
        std::array<std::string_view, 9> row{};
        if (manifest.robustness) {
            row = fields<9>(line);
        } else {
            const auto legacy = fields<8>(line);
            std::copy(legacy.begin(), legacy.end(), row.begin());
        }
        const auto found = positions.find(std::string(row[3]));
        if (found == positions.end()) {
            throw std::runtime_error("Unknown original in augmentation manifest: " + std::string(row[3]));
        }
        const auto index = found->second;
        const auto& sample = originals[index];
        const auto parameter = parseInteger<int>(row[5]);
        auto kind = parameter == 0 ? TransformKind::Original : TransformKind::Gaussian;
        if (manifest.robustness) {
            kind = parseTransform(row[8]);
        }
        const Condition condition{kind, parameter, parseInteger<std::uint64_t>(row[6])};
        const auto seed = parseInteger<std::uint64_t>(row[7]);
        const auto relative = probePath(sample, condition, manifest.robustness);
        if (!validParameter(condition) || row[0] != sample.database
            || parseInteger<unsigned int>(row[1]) != sample.finger
            || parseInteger<unsigned int>(row[2]) != sample.impression
            || row[4] != relative.generic_u8string()) {
            throw std::runtime_error("Augmentation manifest row does not match its FVC sample");
        }
        if (!manifest.robustness) {
            if (manifest.conditions.empty()) {
                legacySeed = condition.baseSeed;
            }
            if (condition.baseSeed != legacySeed) {
                throw std::runtime_error("Noise manifest contains multiple base seeds");
            }
        } else if (kind != TransformKind::Gaussian && (condition.baseSeed != 0 || seed != 0)) {
            throw std::runtime_error("Deterministic transforms require zero seeds in the manifest");
        }
        auto& variant = manifest.conditions.try_emplace(condition, originals.size()).first->second[index];
        if (!variant.relativePath.empty()) {
            throw std::runtime_error("Duplicate FVC image and augmentation condition in manifest");
        }
        variant = {relative, seed};
    }
    const Condition baseline{TransformKind::Original, 0, legacySeed};
    if (!file.eof() || !manifest.conditions.count(baseline) || manifest.conditions.size() < 2) {
        throw std::runtime_error("The manifest needs the baseline and at least one augmentation condition");
    }
    for (const auto& level : manifest.conditions) {
        for (const auto& variant : level.second) {
            if (variant.relativePath.empty()) {
                throw std::runtime_error("Incomplete FVC coverage for " + std::string(transformName(level.first.kind))
                                         + " " + std::to_string(level.first.parameter));
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
