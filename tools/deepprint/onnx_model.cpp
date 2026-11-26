#include "onnx_model.h"

#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace deepprint
{
namespace
{
std::uint64_t varint(const std::string &data, std::size_t &at)
{
    std::uint64_t result = 0;
    for (unsigned shift = 0; shift < 64 && at < data.size(); shift += 7)
    {
        const auto byte = static_cast<unsigned char>(data[at++]);
        if (shift == 63 && byte > 1)
            break;
        result |= std::uint64_t(byte & 127) << shift;
        if (!(byte & 128))
            return result;
    }
    throw std::runtime_error("Invalid protobuf integer");
}
void append(std::string &data, std::uint64_t value)
{
    while (value >= 128)
    {
        data.push_back(static_cast<char>((value & 127) | 128));
        value >>= 7;
    }
    data.push_back(static_cast<char>(value));
}
} // namespace

Message decode(const std::string &data)
{
    Message result;
    for (std::size_t at = 0; at < data.size();)
    {
        const auto tag = varint(data, at);
        if (tag >> 3 == 0 || tag >> 3 > 0x1fffffff)
            throw std::runtime_error("Invalid protobuf tag");
        Field f{static_cast<unsigned>(tag >> 3), static_cast<unsigned>(tag & 7), 0, {}};
        if (!f.wire)
        {
            f.integer = varint(data, at);
        }
        else
        {
            std::uint64_t size;
            switch (f.wire)
            {
            case 1:
                size = 8;
                break;
            case 2:
                size = varint(data, at);
                break;
            case 5:
                size = 4;
                break;
            default:
                throw std::runtime_error("Unsupported protobuf wire type");
            }
            if (size > data.size() - at)
                throw std::runtime_error("Truncated protobuf field");
            f.bytes = data.substr(at, static_cast<std::size_t>(size));
            at += static_cast<std::size_t>(size);
        }
        result.push_back(std::move(f));
    }
    return result;
}

std::string encode(const Message &message)
{
    std::string result;
    std::size_t size = 0;
    for (const auto &f : message)
        size += f.bytes.size() + 12;
    result.reserve(size);
    for (const auto &f : message)
    {
        append(result, std::uint64_t(f.number) * 8 + f.wire);
        if (!f.wire)
            append(result, f.integer);
        else
        {
            if (f.wire == 2)
                append(result, f.bytes.size());
            result += f.bytes;
        }
    }
    return result;
}

Field &field(Message &message, unsigned number)
{
    auto it =
        std::find_if(message.begin(), message.end(), [number](const Field &f) { return f.number == number; });
    if (it == message.end())
        throw std::runtime_error("Missing ONNX field " + std::to_string(number));
    return *it;
}

std::string readFile(const std::filesystem::path &path)
{
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in)
        throw std::runtime_error("Cannot read " + path.u8string());
    const auto length = in.tellg();
    if (length < 0)
        throw std::runtime_error("Unknown file size");
    std::string data(static_cast<size_t>(length), '\0');
    in.seekg(0);
    if (!in.read(data.data(), length))
        throw std::runtime_error("Short read " + path.u8string());
    return data;
}

void writeNew(const std::filesystem::path &path, const std::string &data)
{
    if (std::filesystem::exists(path))
        throw std::runtime_error("Choose a new output path: " + path.u8string());
    if (!path.parent_path().empty())
        std::filesystem::create_directories(path.parent_path());
    static std::atomic<unsigned long long> sequence{};
    auto temporary = path;
    temporary += L"." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64()) +
                 L"." + std::to_wstring(sequence.fetch_add(1)) + L".tmp";
    HANDLE file =
        CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Cannot create new file " + path.u8string());
    bool ok = true;
    for (std::size_t offset = 0; offset < data.size();)
    {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(data.size() - offset, 1 << 20));
        DWORD written = 0;
        if (!WriteFile(file, data.data() + offset, chunk, &written, nullptr) || written != chunk)
        {
            ok = false;
            break;
        }
        offset += written;
    }
    ok = CloseHandle(file) && ok;
    if (!ok || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH))
    {
        DeleteFileW(temporary.c_str());
        throw std::runtime_error("Could not publish new output (check free space and existing files): " +
                                 path.u8string());
    }
}

std::string sha256(const std::filesystem::path &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("Cannot hash " + path.u8string());
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE hash{};
    std::array<unsigned char, 32> digest{};
    bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
              BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) == 0;
    std::array<unsigned char, 65536> block{};
    while (ok && in)
    {
        in.read(reinterpret_cast<char *>(block.data()), block.size());
        ok = BCryptHashData(hash, block.data(), static_cast<ULONG>(in.gcount()), 0) == 0;
    }
    ok = ok && in.eof() && BCryptFinishHash(hash, digest.data(), digest.size(), 0) == 0;
    if (hash)
        BCryptDestroyHash(hash);
    if (algorithm)
        BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok)
        throw std::runtime_error("Cannot hash " + path.u8string());
    std::ostringstream out;
    for (unsigned byte : digest)
        out << std::hex << std::setfill('0') << std::setw(2) << byte;
    return out.str();
}
} // namespace deepprint
