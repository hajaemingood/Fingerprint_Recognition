#pragma once
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace deepprint
{
struct Source
{
    std::string operation, parameter;
    std::size_t channels;
    std::streamoff kernel, gamma, beta, mean, variance;
};

inline std::map<std::string, Source> readLayout(const std::filesystem::path &path)
{
    std::ifstream in(path);
    std::map<std::string, Source> sources;
    std::string line;
    std::getline(in, line);
    while (std::getline(in, line))
    {
        std::istringstream row(line);
        std::string name;
        Source s{};
        if (!(row >> name >> s.operation >> s.parameter >> s.channels >> s.kernel >> s.gamma >> s.beta >>
              s.mean >> s.variance) ||
            !s.channels || !sources.emplace(name, s).second)
            throw std::runtime_error("Invalid parameters.tsv row: " + line);
    }
    if (sources.size() != 394)
        throw std::runtime_error("Incomplete checkpoint layout");
    return sources;
}

inline std::vector<float> load(std::ifstream &stream, std::streamoff offset, std::size_t count)
{
    std::vector<float> data(count);
    stream.seekg(offset);
    if (!stream.read(reinterpret_cast<char *>(data.data()), count * sizeof(float)))
        throw std::runtime_error("Truncated checkpoint tensor");
    return data;
}

} // namespace deepprint
