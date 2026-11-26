#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace deepprint
{
struct Field
{
    unsigned number;
    unsigned wire;
    std::uint64_t integer{};
    std::string bytes;
};
using Message = std::vector<Field>;
Message decode(const std::string &data);
std::string encode(const Message &message);
Field &field(Message &message, unsigned number);
std::string readFile(const std::filesystem::path &path);
void writeNew(const std::filesystem::path &path, const std::string &data);
std::string sha256(const std::filesystem::path &path);
constexpr const char *checkpointHash = "2683a04427bacd54adc00cfdc97474625b1e11e5a9e6672c5129f033018f8a28";
} // namespace deepprint
