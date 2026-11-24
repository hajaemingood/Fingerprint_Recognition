#pragma once

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <map>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fvc {

struct Sample {
    std::string database;
    unsigned int finger;
    unsigned int impression;
    std::filesystem::path relativePath;
};

struct Variant {
    std::filesystem::path relativePath;
    std::uint64_t seed = 0;
};

struct Manifest {
    std::uint64_t baseSeed = 0;
    // Each vector is indexed in the same order as scanDataset.
    std::map<int, std::vector<Variant>> bySigma;
};

inline constexpr std::string_view kManifestHeader =
    "database,finger_id,impression,source,probe,sigma,base_seed,seed";

template <class T>
T parseInteger(std::string_view text)
{
    T value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        throw std::runtime_error("Invalid integer: " + std::string(text));
    }
    return value;
}

std::vector<Sample> scanDataset(const std::filesystem::path& root);
std::filesystem::path probePath(const Sample& sample, int sigma);
Manifest readManifest(const std::filesystem::path& path, const std::vector<Sample>& originals);
void createOutputDirectory(const std::filesystem::path& output);

} // namespace fvc
