#include "cache.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <regex>
#include <sstream>

namespace research
{
namespace
{
using Bytes = std::string;
void integer(Bytes &b, std::uint32_t n, int width)
{
    for (int i = 0; i < width; ++i)
        b.push_back(static_cast<char>(n >> (i * 8)));
}
std::uint32_t integerAt(const Bytes &b, size_t pos, size_t n)
{
    if (pos > b.size() || n > b.size() - pos)
        throw std::runtime_error("Truncated ZIP");
    std::uint32_t result = 0;
    for (size_t i = 0; i < n; ++i)
        result |= std::uint32_t(static_cast<unsigned char>(b[pos + i])) << (8 * i);
    return result;
}
std::uint32_t crc32(const Bytes &b)
{
    std::uint32_t crc = ~0u;
    for (unsigned char c : b)
    {
        crc ^= c;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((0u - (crc & 1)) & 0xedb88320u);
    }
    return ~crc;
}
// NumPy saves uncompressed members, sometimes with ZIP64 local sizes. Sizes and
// offsets for these small per-condition caches are in the central directory.
std::map<std::string, Bytes> unzip(const Bytes &data)
{
    if (data.size() < 22 || integerAt(data, data.size() - 22, 4) != 0x06054b50)
        throw std::runtime_error("Expected NPZ");
    size_t count = integerAt(data, data.size() - 12, 2), at = integerAt(data, data.size() - 6, 4);
    std::map<std::string, Bytes> items;
    for (size_t i = 0; i < count; ++i)
    {
        if (integerAt(data, at, 4) != 0x02014b50 || integerAt(data, at + 10, 2) != 0)
            throw std::runtime_error("Expected stored NPZ");
        auto length = integerAt(data, at + 24, 4), name = integerAt(data, at + 28, 2),
             extra = integerAt(data, at + 30, 2), comment = integerAt(data, at + 32, 2);
        size_t loc = integerAt(data, at + 42, 4);
        size_t start = loc + 30 + integerAt(data, loc + 26, 2) + integerAt(data, loc + 28, 2);
        auto member = data.substr(start, length);
        if (member.size() != length || crc32(member) != integerAt(data, at + 16, 4))
            throw std::runtime_error("Damaged NPZ member");
        if (!items.emplace(data.substr(at + 46, name), std::move(member)).second)
            throw std::runtime_error("Duplicate NPZ member");
        at += 46 + name + extra + comment;
    }
    return items;
}
Bytes zip(const std::map<std::string, Bytes> &files)
{
    Bytes data, dir;
    for (auto &[name, item] : files)
    {
        auto offset = data.size();
        Bytes attrs;
        integer(attrs, 20, 2);
        integer(attrs, 0, 2);
        integer(attrs, 0, 2);
        integer(attrs, 0, 4);
        integer(attrs, crc32(item), 4);
        integer(attrs, item.size(), 4);
        integer(attrs, item.size(), 4);
        integer(attrs, name.size(), 2);
        integer(attrs, 0, 2);
        integer(data, 0x04034b50, 4);
        data += attrs + name + item;
        integer(dir, 0x02014b50, 4);
        integer(dir, 20, 2);
        dir += attrs;
        integer(dir, 0, 2);
        integer(dir, 0, 2);
        integer(dir, 0, 2);
        integer(dir, 0, 4);
        integer(dir, offset, 4);
        dir += name;
    }
    auto offset = data.size();
    data += dir;
    integer(data, 0x06054b50, 4);
    integer(data, 0, 4);
    integer(data, files.size(), 2);
    integer(data, files.size(), 2);
    integer(data, dir.size(), 4);
    integer(data, offset, 4);
    integer(data, 0, 2);
    return data;
}
Bytes npy(const std::string &type, const std::string &shape, const Bytes &raw)
{
    std::string header = "{'descr': '" + type + "', 'fortran_order': False, 'shape': (" + shape + "), }";
    header.append((64 - (10 + header.size() + 1) % 64) % 64, ' ');
    header += '\n';
    Bytes data = "\x93NUMPY\x01";
    data.push_back(0);
    integer(data, header.size(), 2);
    return data + header + raw;
}
struct Array
{
    std::string header, raw;
};
Array parse(const Bytes &b)
{
    if (b.size() < 10 || b.compare(0, 6, "\x93NUMPY"))
        throw std::runtime_error("Expected NPY");
    auto skip = b[6] == 1 ? 10 : 12;
    auto len = integerAt(b, 8, skip - 8);
    return {b.substr(skip, len), b.substr(skip + len)};
}
std::string padded(int n, bool sign)
{
    std::ostringstream s;
    if (sign)
        s << (n < 0 ? '-' : '+');
    s << std::setfill('0') << std::setw(2) << std::abs(n);
    return s.str();
}
void saveCache(const fs::path &file, const EmbeddingBank &b)
{
    std::string names, features, timings;
    size_t width = 0;
    for (auto &n : b.names)
        width = std::max(width, n.size());
    for (size_t i = 0; i < b.names.size(); ++i)
    {
        for (size_t j = 0; j < width; ++j)
            integer(names, j < b.names[i].size() ? static_cast<unsigned char>(b.names[i][j]) : 0, 4);
        features.append(reinterpret_cast<const char *>(b.vectors[i].data()), 512 * 4);
        timings.append(reinterpret_cast<const char *>(&b.timings[i]), 8);
    }
    auto n = std::to_string(b.names.size());
    deepprint::writeNew(file, zip({{"names.npy", npy("<U" + std::to_string(width), n + ",", names)},
                                   {"embeddings.npy", npy("<f4", n + ", 512", features)},
                                   {"attempt_ms.npy", npy("<f8", n + ",", timings)}}));
}
} // namespace

EmbeddingBank readCache(const fs::path &path)
{
    auto members = unzip(deepprint::readFile(path));
    auto names = parse(members.at("names.npy")), v = parse(members.at("embeddings.npy")),
         t = parse(members.at("attempt_ms.npy"));
    if (v.header.find("'<f4'") == std::string::npos || t.header.find("'<f8'") == std::string::npos ||
        v.raw.size() % 2048 || v.raw.empty())
        throw std::runtime_error("Invalid cache vector format");
    size_t n = v.raw.size() / 2048, width = names.raw.size() / n;
    if (width % 4 || width * n != names.raw.size() || t.raw.size() != 8 * n ||
        names.header.find("'<U") == std::string::npos)
        throw std::runtime_error("Invalid cache names/timings");
    EmbeddingBank b;
    b.vectors.resize(n);
    b.timings.resize(n);
    std::memcpy(b.vectors.data(), v.raw.data(), v.raw.size());
    std::memcpy(b.timings.data(), t.raw.data(), t.raw.size());
    for (size_t i = 0; i < n; ++i)
    {
        std::string name;
        for (size_t j = 0; j < width; j += 4)
        {
            auto ch = integerAt(names.raw, i * width + j, 4);
            if (!ch)
                break;
            if (ch > 127)
                throw std::runtime_error("Invalid FVC sample name");
            name.push_back(static_cast<char>(ch));
        }
        b.names.push_back(name);
    }
    return b;
}

std::string conditionName(const Row &row)
{
    int v = std::stoi(row.count("parameter") ? row.at("parameter") : row.at("sigma"));
    std::string kind = row.count("transform") ? row.at("transform") : (v ? "gaussian" : "original");
    if (kind == "gaussian")
        return "noise-" + padded(v, false) + "-s" + padded(std::stoi(row.at("base_seed")), false);
    return kind == "original" ? kind : kind + padded(v, true);
}
std::vector<std::vector<std::string>> settings(int maxSigma)
{
    const auto profile = deepprint::readFile("src/experiments/fvc_augmentation.json");
    auto list = [&](const std::string &key) {
        std::smatch match;
        if (!std::regex_search(profile, match, std::regex("\"" + key + "\"\\s*:\\s*\\[([^\\]]+)\\]")))
            throw std::runtime_error("Missing augmentation setting " + key);
        std::istringstream values(match[1].str());
        std::string token;
        std::vector<int> result;
        while (std::getline(values, token, ','))
            result.push_back(std::stoi(token));
        return result;
    };
    std::vector<std::vector<std::string>> result;
    auto sigmas = list("gaussian_sigmas"), seeds = list("gaussian_seeds");
    if (maxSigma == 40 && std::find(sigmas.begin(), sigmas.end(), 40) == sigmas.end())
        sigmas.push_back(40);
    for (int sigma : sigmas)
        if (sigma <= maxSigma)
        {
            std::vector<std::string> setting;
            for (int seed : seeds)
                setting.push_back("noise-" + padded(sigma, false) + "-s" + padded(seed, false));
            result.push_back(std::move(setting));
        }
    for (int r : list("rotation_degrees"))
        result.push_back({"rotation" + padded(r, true)});
    for (int b : list("brightness_offsets"))
        result.push_back({"brightness" + padded(b, true)});
    return result;
}

void cacheEmbeddings(const fs::path &model, const fs::path &root, const fs::path &output,
                     const std::vector<fs::path> &manifests, int maxSigma, bool standardOnly)
{
    using Mapping = std::map<std::string, fs::path>;
    std::map<std::string, std::map<std::string, Mapping>> groups;
    for (auto &s : fvc::scanDataset(root))
        if (s.database != "FVC2000_DB3_B")
            groups[s.database]["original"][s.relativePath.filename().u8string()] = root / s.relativePath;
    std::ostringstream identity;
    identity << deepprint::sha256(model) << '\n'
             << fs::absolute(root).u8string() << '\n'
             << maxSigma << ',' << standardOnly << '\n';
    for (auto dir : manifests)
    {
        auto manifest = dir / "manifest.csv";
        identity << fs::absolute(dir).u8string() << ',' << deepprint::sha256(manifest) << '\n';
        for (auto &row : csv(manifest))
        {
            auto db = row.at("database"), key = conditionName(row);
            if (!groups.count(db) || key == "original" || key.rfind("flip", 0) == 0 ||
                (key.rfind("noise-", 0) == 0 && std::stoi(key.substr(6)) > maxSigma))
                continue;
            if (!groups[db][key]
                     .emplace(basename(row.at("source")), dir / fs::u8path(row.at("probe")))
                     .second)
                throw std::runtime_error("Duplicate augmentation: " + db + "/" + key);
        }
    }
    auto marker = output / "source.txt";
    if (fs::exists(marker))
    {
        if (deepprint::readFile(marker) != identity.str())
            throw std::runtime_error("Cache source mismatch");
    }
    else
    {
        if (fs::is_directory(output) && !fs::is_empty(output))
            throw std::runtime_error("Choose an empty cache directory or a cache with matching provenance");
        deepprint::writeNew(marker, identity.str());
    }
    Inference engine(model);
    for (auto &[db, conditions] : groups)
    {
        auto expected = settings(maxSigma);
        for (auto &setting : expected)
            for (auto &key : setting)
                if (!conditions.count(key))
                    throw std::runtime_error("Missing augmentation " + db + "/" + key);
        for (auto &[key, paths] : conditions)
        {
            if (paths.size() != conditions.at("original").size())
                throw std::runtime_error("Incomplete condition " + key);
            std::map<std::string, std::string> modes{{"standard", "standard"}};
            if (!standardOnly && db == "FVC2002_DB2_B" &&
                (key == "original" || key == "noise-10-s42" ||
                 key == "noise-" + padded(maxSigma, false) + "-s42"))
                modes["bilateral"] = "bilateral-12";
            if (!standardOnly && db == "FVC2004_DB1_B" &&
                (key == "original" || key == "rotation-10" || key == "rotation+10"))
            {
                modes["roi"] = "roi-10";
                modes["center"] = "center";
            }
            for (auto &[label, mode] : modes)
            {
                auto file = output / db / label / (key + ".npz");
                if (fs::exists(file))
                {
                    readCache(file);
                    continue;
                }
                EmbeddingBank b;
                for (auto &[name, path] : paths)
                {
                    if (!conditions.at("original").count(name))
                        throw std::runtime_error("Unknown capture " + name);
                    auto start = std::chrono::steady_clock::now();
                    auto input = imageInput(fvc::readTiff(path), mode);
                    auto v = engine.run(input, {"embedding"}).at("embedding");
                    if (v.size() != 512)
                        throw std::runtime_error("Invalid embedding length");
                    b.names.push_back(name);
                    b.vectors.emplace_back();
                    std::copy(v.begin(), v.end(), b.vectors.back().begin());
                    b.timings.push_back(
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                            .count());
                }
                saveCache(file, b);
                std::cout << file.u8string() << std::endl;
            }
        }
    }
}
} // namespace research
