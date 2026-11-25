#include "fingerprint_image.h"
#include "image_format.h"
#include "kisa_api.h"

#include <windows.h>
#include <wincodec.h>
#include <memory>
#include <utility>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <map>
#include <random>
#include <set>
#include <stdexcept>
#include <tuple>
#include <fstream>
#include <sstream>
#include <array>
#include <iomanip>
#include <mutex>
#include <thread>
#include <exception>
#include <condition_variable>
#include <psapi.h>

namespace
{

struct ReleaseCom
{
    void operator()(IUnknown *object) const { object->Release(); }
};

template <class T> using ComPtr = std::unique_ptr<T, ReleaseCom>;

bool fail(std::wstring &error, const wchar_t *message)
{
    error = message;
    return false;
}

} // namespace

bool loadFingerprintImage(const wchar_t *path, FingerprintImage &image, std::wstring &error)
{
    IWICImagingFactory *factoryRaw = nullptr;
    const HRESULT initialized = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                                 CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factoryRaw));
    if (FAILED(initialized))
    {
        return fail(error, L"Windows 이미지 디코더를 시작할 수 없습니다.");
    }
    const ComPtr<IWICImagingFactory> factory(factoryRaw);

    IWICBitmapDecoder *decoderRaw = nullptr;
    if (FAILED(factory->CreateDecoderFromFilename(path, nullptr, GENERIC_READ,
                                                  WICDecodeMetadataCacheOnDemand, &decoderRaw)))
    {
        return fail(error, L"이미지 파일을 열 수 없습니다.");
    }
    const ComPtr<IWICBitmapDecoder> decoder(decoderRaw);

    GUID container{};
    if (FAILED(decoder->GetContainerFormat(&container)) || container != GUID_ContainerFormatTiff)
    {
        return fail(error, L"TIFF(.tif, .tiff) 파일을 선택하세요.");
    }

    IWICBitmapFrameDecode *frameRaw = nullptr;
    if (FAILED(decoder->GetFrame(0, &frameRaw)))
    {
        return fail(error, L"TIFF의 첫 번째 영상을 읽을 수 없습니다.");
    }
    const ComPtr<IWICBitmapFrameDecode> frame(frameRaw);

    UINT width = 0;
    UINT height = 0;
    if (FAILED(frame->GetSize(&width, &height)))
    {
        return fail(error, L"영상 크기를 읽을 수 없습니다.");
    }
    FingerprintImage decoded;
    for (const int type : {1, 2, 3, 5, 7, 8, 9})
    {
        const auto format = fingerprint::imageFormat(type);
        if (width == static_cast<UINT>(format.width) && height == static_cast<UINT>(format.height))
        {
            decoded.type = type;
            break;
        }
    }
    if (decoded.type == 0)
    {
        return fail(error, L"공통 image_format.h에 정의된 영상 크기가 아닙니다.");
    }

    IWICFormatConverter *converterRaw = nullptr;
    if (FAILED(factory->CreateFormatConverter(&converterRaw)))
    {
        return fail(error, L"회색조 변환기를 시작할 수 없습니다.");
    }
    const ComPtr<IWICFormatConverter> converter(converterRaw);
    if (FAILED(converter->Initialize(frame.get(), GUID_WICPixelFormat8bppGray,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom)))
    {
        return fail(error, L"영상을 8비트 회색조로 변환할 수 없습니다.");
    }

    decoded.pixels.resize(width * height);
    if (FAILED(converter->CopyPixels(nullptr, width, width * height, decoded.pixels.data())))
    {
        return fail(error, L"TIFF 픽셀을 읽을 수 없습니다.");
    }
    image = std::move(decoded);
    error.clear();
    return true;
}

namespace demo
{
namespace
{
std::wstring apiError(const wchar_t *stage, int code, const char *message)
{
    // The current DLL returns ASCII messages; convert bounded output for the UI.
    const auto end = std::find(message, message + 256, '\0');
    int count =
        MultiByteToWideChar(CP_ACP, 0, message, static_cast<int>(end - message), nullptr, 0);
    std::wstring detail(count, L' ');
    if (count)
        MultiByteToWideChar(CP_ACP, 0, message, static_cast<int>(end - message), detail.data(),
                            count);
    return std::wstring(stage) + L" 실패 (" + std::to_wstring(code) + L") " + detail;
}
double ratio(size_t count, size_t total)
{
    return total ? static_cast<double>(count) / total : NAN;
}
} // namespace

Plan buildPlan(const Settings &settings)
{
    if (!fs::is_directory(settings.database))
        throw std::runtime_error("테스트 DB 폴더를 선택하세요.");
    if (settings.randomCount < 1 || settings.registrationIndex < 1 || settings.genuineIndex < 1 ||
        settings.impostorIndex < 1 || settings.parallelism < 1 || settings.parallelism > 16)
        throw std::runtime_error("촬영번호와 무작위 개수는 1 이상이어야 합니다.");
    Plan plan;
    plan.settings = settings;
    std::set<std::pair<int, int>> ids;
    for (const auto &file : fs::directory_iterator(settings.database))
    {
        if (!file.is_regular_file())
            continue;
        auto ext = file.path().extension().wstring();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
        if (ext != L".tif" && ext != L".tiff")
            continue;
        auto stem = file.path().stem().wstring();
        auto split = stem.find(L'_');
        auto digits = [](const std::wstring &value)
        { return !value.empty() && value.find_first_not_of(L"0123456789") == std::wstring::npos; };
        if (split == std::wstring::npos || !digits(stem.substr(0, split)) ||
            !digits(stem.substr(split + 1)))
            throw std::runtime_error(
                "TIFF 파일명은 손가락번호_촬영번호 형식이어야 합니다. 예: 101_1.tif");
        ImageFile image{fs::weakly_canonical(file.path()), std::stoi(stem.substr(0, split)),
                        std::stoi(stem.substr(split + 1))};
        if (image.finger <= 0 || image.impression <= 0 ||
            !ids.insert({image.finger, image.impression}).second)
            throw std::runtime_error("중복되거나 올바르지 않은 손가락·촬영번호가 있습니다.");
        plan.images.push_back(std::move(image));
    }
    if (plan.images.size() < 2)
        throw std::runtime_error("한 FVC DB의 TIFF 이미지가 두 장 이상 필요합니다.");
    std::sort(plan.images.begin(), plan.images.end(), [](const auto &a, const auto &b)
              { return std::tie(a.finger, a.impression) < std::tie(b.finger, b.impression); });
    std::map<int, std::vector<size_t>> groups;
    for (size_t i = 0; i < plan.images.size(); ++i)
        groups[plan.images[i].finger].push_back(i);
    plan.fingers = groups.size();
    std::mt19937 random(settings.seed);
    std::vector<size_t> registrations;
    for (const auto &[finger, candidates] : groups)
    {
        if (settings.registration == RegistrationRule::All)
            registrations.insert(registrations.end(), candidates.begin(), candidates.end());
        else if (settings.registration == RegistrationRule::First)
            registrations.push_back(candidates.front());
        else if (settings.registration == RegistrationRule::Random)
            registrations.push_back(candidates[std::uniform_int_distribution<size_t>(
                0, candidates.size() - 1)(random)]);
        else
        {
            auto it =
                std::find_if(candidates.begin(), candidates.end(), [&](size_t i)
                             { return plan.images[i].impression == settings.registrationIndex; });
            if (it == candidates.end())
                throw std::runtime_error("일부 손가락에 지정한 등록 촬영번호가 없습니다.");
            registrations.push_back(*it);
        }
    }
    plan.registrations = registrations.size();
    for (size_t registered : registrations)
    {
        for (bool genuine : {true, false})
        {
            auto rule = genuine ? settings.genuine : settings.impostor;
            int index = genuine ? settings.genuineIndex : settings.impostorIndex;
            std::vector<size_t> candidates;
            for (const auto &[finger, group] : groups)
            {
                if ((finger == plan.images[registered].finger) != genuine)
                    continue;
                for (size_t attempt : group)
                {
                    if (attempt == registered)
                        continue;
                    if (rule == ComparisonRule::Impression &&
                        plan.images[attempt].impression != index)
                        continue;
                    candidates.push_back(attempt);
                    if (rule == ComparisonRule::First)
                        break;
                }
            }
            if (rule == ComparisonRule::Random)
            {
                std::shuffle(candidates.begin(), candidates.end(), random);
                if (candidates.size() > static_cast<size_t>(settings.randomCount))
                    candidates.resize(settings.randomCount);
            }
            for (size_t attempt : candidates)
                plan.pairs.push_back({registered, attempt, genuine});
        }
    }
    if (plan.pairs.empty())
        throw std::runtime_error("규칙에 맞는 비교 쌍이 없습니다. 촬영번호를 확인하세요.");
    std::shuffle(plan.pairs.begin(), plan.pairs.end(), random);
    return plan;
}

namespace
{
std::string csvField(const std::string &value)
{
    std::string out = "\"";
    for (char ch : value)
    {
        if (ch == '"')
            out += '"';
        out += ch;
    }
    return out + '"';
}
bool csvRow(std::istream &in, std::vector<std::string> &row)
{
    row.clear();
    std::string field;
    bool quoted = false, closed = false, started = false;
    char ch;
    while (in.get(ch))
    {
        started = true;
        if (quoted)
        {
            if (ch == '"')
            {
                if (in.peek() == '"')
                {
                    in.get();
                    field += '"';
                }
                else
                {
                    quoted = false;
                    closed = true;
                }
            }
            else
                field += ch;
        }
        else if (ch == ',')
        {
            row.push_back(field);
            field.clear();
            closed = false;
        }
        else if (ch == '\r' || ch == '\n')
        {
            if (ch == '\r' && in.peek() == '\n')
                in.get();
            row.push_back(field);
            return true;
        }
        else if (ch == '"' && field.empty() && !closed)
            quoted = true;
        else
        {
            if (closed || ch == '"')
                throw std::runtime_error("CSV 따옴표 형식이 올바르지 않습니다.");
            field += ch;
        }
        if (field.size() > 65536 || row.size() > 16)
            throw std::runtime_error("CSV 필드가 너무 큽니다.");
    }
    if (quoted)
        throw std::runtime_error("CSV의 따옴표가 닫히지 않았습니다.");
    if (started)
        row.push_back(field);
    return started;
}
int csvInteger(const std::string &text, int minimum, int maximum)
{
    size_t end = 0;
    long long value = 0;
    try
    {
        value = std::stoll(text, &end);
    }
    catch (...)
    {
        throw std::runtime_error("CSV 설정의 숫자를 확인하세요.");
    }
    if (end != text.size() || value < minimum || value > maximum)
        throw std::runtime_error("CSV 설정의 숫자 범위를 확인하세요.");
    return static_cast<int>(value);
}
fs::path currentDatabasePath(fs::path path)
{
    if (!fs::exists(path) && path.filename() == L"FVC2002_DB3_B" &&
        path.parent_path().filename() == L"fvc")
    {
        auto renamed = path.parent_path() / L"FVC2002_DB2_B";
        if (fs::is_directory(renamed))
            return renamed;
    }
    return path;
}
ImageFile csvImage(fs::path path)
{
    path = currentDatabasePath(path.parent_path()) / path.filename();
    if (!fs::is_regular_file(path))
        throw std::runtime_error("CSV에 지정한 지문 파일이 없습니다.");
    auto extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), ::towlower);
    if (extension != L".tif" && extension != L".tiff")
        throw std::runtime_error("CSV에는 TIFF 파일만 지정할 수 있습니다.");
    auto name = path.stem().u8string();
    auto underscore = name.find('_');
    auto digits = [](const std::string &value)
    { return !value.empty() && value.find_first_not_of("0123456789") == std::string::npos; };
    if (underscore == std::string::npos || !digits(name.substr(0, underscore)) ||
        !digits(name.substr(underscore + 1)))
        throw std::runtime_error("CSV 파일명은 손가락번호_촬영번호 형식이어야 합니다.");
    return {fs::weakly_canonical(path), csvInteger(name.substr(0, underscore), 1, 2147483647),
            csvInteger(name.substr(underscore + 1), 1, 2147483647)};
}
} // namespace

void savePlan(const Plan &plan, const fs::path &file)
{
    if (plan.pairs.empty())
        throw std::runtime_error("먼저 비교 목록을 만드세요.");
    std::ofstream out(file, std::ios::binary);
    if (!out)
        throw std::runtime_error("비교 목록 저장 파일을 열지 못했습니다.");
    const auto &s = plan.settings;
    out << "\xEF\xBB\xBF" << "#fingerprint_demo,1\r\n";
    auto storedDatabase =
        fs::absolute(s.database).lexically_relative(fs::absolute(file).parent_path());
    if (storedDatabase.empty())
        storedDatabase = fs::absolute(s.database);
    out << "#database," << csvField(storedDatabase.u8string()) << "\r\n";
    out << "#settings," << static_cast<int>(s.registration) << ',' << static_cast<int>(s.genuine)
        << ',' << static_cast<int>(s.impostor) << ',' << s.registrationIndex << ','
        << s.genuineIndex << ',' << s.impostorIndex << ',' << s.randomCount << ',' << s.seed << ','
        << s.parallelism << "\r\n";
    out << "#reuse_features," << (s.reuseFeatures ? 1 : 0) << "\r\n";
    out << "enroll,attempt,relation\r\n";
    // Save the actual directed pairs in display order, not just a random seed.
    for (const auto &p : plan.pairs)
    {
        out << csvField(plan.images.at(p.registration).path.filename().u8string()) << ','
            << csvField(plan.images.at(p.attempt).path.filename().u8string()) << ','
            << (p.genuine ? "genuine" : "impostor") << "\r\n";
    }
    if (!out)
        throw std::runtime_error("비교 목록을 끝까지 저장하지 못했습니다.");
}

Plan loadPlan(const fs::path &file, const Settings &fallback)
{
    std::ifstream in(file, std::ios::binary);
    if (!in)
        throw std::runtime_error("비교 목록 CSV를 열지 못했습니다.");
    // Strip only the optional UTF-8 BOM, before parsing quoted CSV fields.
    char bom[3]{};
    in.read(bom, 3);
    if (in.gcount() != 3 || std::string(bom, 3) != "\xEF\xBB\xBF")
    {
        in.clear();
        in.seekg(0);
    }
    Plan plan;
    plan.settings = fallback;
    plan.sourceFile = fs::absolute(file);
    std::vector<std::string> row;
    bool header = false, databaseMetadata = false;
    std::set<std::pair<size_t, size_t>> seen;
    std::set<int> fingers;
    std::set<size_t> registrations;
    std::map<fs::path, size_t> images;
    std::map<std::pair<int, int>, fs::path> identities;
    fs::path actualDatabase, originalDatabase;
    bool relocatedDatabase = false;
    auto addImage = [&](const std::string &value)
    {
        if (value.empty())
            throw std::runtime_error("CSV에 빈 이미지 경로가 있습니다.");
        auto path = fs::u8path(value);
        if (!path.is_absolute())
            path = plan.settings.database / path;
        if (relocatedDatabase && path.is_absolute() && path.parent_path() == originalDatabase)
            path = plan.settings.database / path.filename();
        auto image = csvImage(path);
        if (actualDatabase.empty())
            actualDatabase = image.path.parent_path();
        if (!fs::equivalent(actualDatabase, image.path.parent_path()))
            throw std::runtime_error("한 CSV에서는 같은 DB 폴더의 지문만 비교하세요.");
        if (databaseMetadata && !fs::equivalent(plan.settings.database, image.path.parent_path()))
            throw std::runtime_error("CSV의 DB 설정과 이미지 경로가 일치하지 않습니다.");
        const auto key = std::make_pair(image.finger, image.impression);
        auto existing = identities.find(key);
        if (existing != identities.end() && !fs::equivalent(existing->second, image.path))
            throw std::runtime_error("같은 손가락·촬영번호에 서로 다른 파일이 지정되어 있습니다.");
        identities[key] = image.path;
        auto existingImage = images.find(image.path);
        if (existingImage != images.end())
            return existingImage->second;
        size_t index = plan.images.size();
        images[image.path] = index;
        fingers.insert(image.finger);
        plan.images.push_back(std::move(image));
        return index;
    };
    while (csvRow(in, row))
    {
        if (row.size() == 1 && row[0].empty())
            continue;
        if (!header && !row.empty() && !row[0].empty() && row[0][0] == '#')
        {
            if (row[0] == "#fingerprint_demo" && (row.size() != 2 || row[1] != "1"))
                throw std::runtime_error("지원하지 않는 비교 목록 버전입니다.");
            if (row[0] == "#database")
            {
                if (row.size() != 2 || row[1].empty())
                    throw std::runtime_error("CSV의 DB 경로가 비어 있습니다.");
                auto path = fs::u8path(row[1]);
                originalDatabase = path;
                plan.settings.database = fs::weakly_canonical(
                    currentDatabasePath(path.is_absolute() ? path : file.parent_path() / path));
                if (!fs::is_directory(plan.settings.database))
                {
                    if (!fs::is_directory(fallback.database) ||
                        fallback.database.filename() != path.filename())
                        throw std::runtime_error(
                            "CSV의 DB를 찾을 수 없습니다. 먼저 같은 이름의 DB 폴더를 선택하세요.");
                    plan.settings.database = fs::canonical(fallback.database);
                    relocatedDatabase = true;
                }
                databaseMetadata = true;
            }
            if (row[0] == "#reuse_features")
            {
                if (row.size() != 2)
                    throw std::runtime_error("CSV의 특징 재사용 설정을 확인하세요.");
                plan.settings.reuseFeatures = csvInteger(row[1], 0, 1) != 0;
            }
            if (row[0] == "#settings")
            {
                if (row.size() != 10)
                    throw std::runtime_error("CSV 설정 열의 개수가 올바르지 않습니다.");
                auto &s = plan.settings;
                s.registration = static_cast<RegistrationRule>(csvInteger(row[1], 0, 3));
                s.genuine = static_cast<ComparisonRule>(csvInteger(row[2], 0, 3));
                s.impostor = static_cast<ComparisonRule>(csvInteger(row[3], 0, 3));
                s.registrationIndex = csvInteger(row[4], 1, 9999);
                s.genuineIndex = csvInteger(row[5], 1, 9999);
                s.impostorIndex = csvInteger(row[6], 1, 9999);
                s.randomCount = csvInteger(row[7], 1, 100000);
                s.seed = static_cast<unsigned>(csvInteger(row[8], 0, 2147483647));
                s.parallelism = csvInteger(row[9], 1, 16);
            }
            continue;
        }
        if (!header)
        {
            if (row != std::vector<std::string>{"enroll", "attempt", "relation"})
                throw std::runtime_error(
                    "CSV 첫 데이터 행은 enroll,attempt,relation 이어야 합니다.");
            header = true;
            continue;
        }
        if (row.size() != 3 || (row[2] != "genuine" && row[2] != "impostor"))
            throw std::runtime_error(
                "CSV 행은 등록 경로, 비교 경로, genuine 또는 impostor 이어야 합니다.");
        size_t registration = addImage(row[0]), attempt = addImage(row[1]);
        if (registration == attempt ||
            fs::equivalent(plan.images[registration].path, plan.images[attempt].path))
            throw std::runtime_error("동일 이미지끼리의 비교는 허용하지 않습니다.");
        bool genuine = plan.images[registration].finger == plan.images[attempt].finger;
        if (genuine != (row[2] == "genuine"))
            throw std::runtime_error("CSV의 본인·타인 표기가 실제 손가락 번호와 다릅니다.");
        if (!seen.insert({registration, attempt}).second)
            throw std::runtime_error("CSV에 동일한 방향의 비교 쌍이 중복되어 있습니다.");
        if (plan.pairs.size() >= 1000000)
            throw std::runtime_error("CSV 비교 목록은 100만 쌍 이하로 작성하세요.");
        plan.pairs.push_back({registration, attempt, genuine});
        registrations.insert(registration);
    }
    if (plan.pairs.empty())
        throw std::runtime_error("CSV에 실행할 비교 쌍이 없습니다.");
    plan.settings.database = actualDatabase;
    plan.registrations = registrations.size();
    plan.fingers = fingers.size();
    return plan; // Do not shuffle or regenerate an imported list.
}

namespace
{
fs::path backendDirectory()
{
    std::wstring path(32768, L'\0');
    DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size())
        throw std::runtime_error("실행 파일 위치를 확인할 수 없습니다.");
    path.resize(length);
    return fs::path(path).parent_path();
}
#ifdef FINGERPRINT_DYNAMIC_BACKEND
// DeepPrint locates its runtime/model relative to the loaded DLL itself.
// A private, versioned copy lets an external replacement reuse the app's assets
// without modifying its source folder or an already-loaded (pinned) DLL.
fs::path prepareBackend(const fs::path &selected)
{
    const auto source = fs::weakly_canonical(selected);
    std::ifstream input(source, std::ios::binary);
    if (!input)
        throw std::runtime_error("선택한 DLL 파일을 읽을 수 없습니다.");
    std::string contents((std::istreambuf_iterator<char>(input)), {});
    const std::wstring modelName = L"deepprint_texminu_512.onnx";
    const std::string wideMarker(reinterpret_cast<const char *>(modelName.data()),
                                 modelName.size() * sizeof(wchar_t));
    if (contents.find("deepprint_texminu_512.onnx") == std::string::npos &&
        contents.find(wideMarker) == std::string::npos)
        return source; // Other KISA algorithms keep their own dependency layout.
    const auto originalFolder = source.parent_path();
    const auto appFolder = backendDirectory();
    std::vector<fs::path> assets;
    bool needsFallback = false;
    for (const auto *name : {L"onnxruntime.dll", L"deepprint_texminu_512.onnx"})
    {
        auto asset = originalFolder / name;
        if (!fs::is_regular_file(asset))
        {
            asset = appFolder / name;
            needsFallback = true;
        }
        if (!fs::is_regular_file(asset))
            throw std::runtime_error("DeepPrint 모델 또는 런타임이 없습니다. onnxruntime.dll과 "
                                     "deepprint_texminu_512.onnx를 실행 파일 폴더에 준비하세요.");
        assets.push_back(asset);
    }
    if (!needsFallback)
        return source;
    // Include adjacent DLL dependencies as well as the known model/runtime.
    for (const auto &entry : fs::directory_iterator(originalFolder))
    {
        auto extension = entry.path().extension().wstring();
        std::transform(extension.begin(), extension.end(), extension.begin(), ::towlower);
        if (entry.is_regular_file() && extension == L".dll" && entry.path() != source &&
            entry.path().filename() != L"onnxruntime.dll")
            assets.push_back(entry.path());
    }
    std::sort(assets.begin(), assets.end());
    unsigned long long hash = 14695981039346656037ull;
    auto append = [&](const std::string &value)
    {
        for (unsigned char c : value)
        {
            hash ^= c;
            hash *= 1099511628211ull;
        }
    };
    append(contents);
    append(source.u8string());
    for (const auto &asset : assets)
    {
        append(asset.u8string());
        append(std::to_string(fs::file_size(asset)));
        append(std::to_string(fs::last_write_time(asset).time_since_epoch().count()));
    }
    std::ostringstream key;
    key << std::hex << hash;
    const auto directory =
        fs::temp_directory_path() / L"FingerprintLab" / L"backends" / fs::u8path(key.str());
    fs::create_directories(directory);
    auto copy = [&](const fs::path &from)
    {
        auto to = directory / from.filename();
        if (fs::is_regular_file(to) && fs::file_size(from) == fs::file_size(to))
            return;
        // Publish only complete copies; interrupted copies cannot become a valid cache entry.
        GUID id{};
        if (FAILED(CoCreateGuid(&id)))
            throw std::runtime_error("DLL 실행 폴더를 준비하지 못했습니다.");
        wchar_t token[40]{};
        StringFromGUID2(id, token, 40);
        auto temporary = directory / token;
        try
        {
            fs::copy_file(from, temporary);
            fs::rename(temporary, to);
        }
        catch (...)
        {
            std::error_code ignored;
            fs::remove(temporary, ignored);
            throw;
        }
    };
    for (const auto &asset : assets)
        copy(asset);
    copy(source);
    return directory / source.filename();
}
struct Backend
{
    HMODULE library = nullptr;
    decltype(&KISA_Enroll) enroll = nullptr;
    decltype(&KISA_Attempt) attempt = nullptr;
    decltype(&KISA_Compare) compare = nullptr;
    explicit Backend(const fs::path &selected)
    {
        if (selected.empty())
            throw std::runtime_error("먼저 알고리즘 DLL을 선택하세요.");
        const auto original = fs::absolute(selected);
        const auto dll = original;
        std::ifstream input(dll, std::ios::binary);
        IMAGE_DOS_HEADER dos{};
        input.read(reinterpret_cast<char *>(&dos), sizeof(dos));
        if (!input || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < sizeof(dos))
            throw std::runtime_error("올바른 Windows DLL 파일이 아닙니다.");
        input.seekg(dos.e_lfanew);
        DWORD signature = 0;
        IMAGE_FILE_HEADER header{};
        input.read(reinterpret_cast<char *>(&signature), sizeof(signature));
        input.read(reinterpret_cast<char *>(&header), sizeof(header));
        if (!input || signature != IMAGE_NT_SIGNATURE || !(header.Characteristics & IMAGE_FILE_DLL))
            throw std::runtime_error("올바른 Windows DLL 파일이 아닙니다.");
        if (header.Machine != IMAGE_FILE_MACHINE_AMD64)
            throw std::runtime_error(
                "이 앱은 x64 DLL이 필요합니다. 32비트 또는 다른 아키텍처 DLL입니다.");
        const auto loadPath = prepareBackend(original);
        library = LoadLibraryExW(loadPath.c_str(), nullptr,
                                 LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!library)
            throw std::runtime_error("선택한 DLL을 읽을 수 없습니다. DLL과 같은 폴더에 필요한 종속 "
                                     "DLL과 모델 파일을 준비하세요. "
                                     "Windows 오류: " +
                                     std::to_string(GetLastError()));
        enroll = reinterpret_cast<decltype(enroll)>(GetProcAddress(library, "KISA_Enroll"));
        attempt = reinterpret_cast<decltype(attempt)>(GetProcAddress(library, "KISA_Attempt"));
        compare = reinterpret_cast<decltype(compare)>(GetProcAddress(library, "KISA_Compare"));
        if (!enroll || !attempt || !compare)
        {
            FreeLibrary(library);
            library = nullptr;
            throw std::runtime_error("선택한 DLL에 KISA 등록·추출·비교 함수가 없습니다.");
        }
    }
    ~Backend()
    {
        if (library)
            FreeLibrary(library);
    }
    Backend(const Backend &) = delete;
    Backend &operator=(const Backend &) = delete;
};
#endif
} // namespace
std::wstring recognitionBackendLabel(const fs::path &algorithm)
{
    return algorithm.empty() ? L"알고리즘 DLL 미선택" : algorithm.filename().wstring();
}
void validateAlgorithm(const fs::path &algorithm)
{
#ifdef FINGERPRINT_DYNAMIC_BACKEND
    Backend checked(algorithm);
#else
    (void)algorithm;
#endif
}

namespace
{
// Phase-specific, sampled working set of the whole app, not isolated DLL allocations.
struct MemorySampler
{
    std::atomic<int> phase{0};
    std::atomic<size_t> enrollment{0}, matching{0};
    std::mutex mutex;
    std::condition_variable wake;
    bool done = false;
    std::thread thread;
    void sample()
    {
        using Query = BOOL(WINAPI *)(HANDLE, PPROCESS_MEMORY_COUNTERS, DWORD);
        static auto query = reinterpret_cast<Query>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32GetProcessMemoryInfo"));
        PROCESS_MEMORY_COUNTERS counters{};
        counters.cb = sizeof(counters);
        if (!query || !query(GetCurrentProcess(), &counters, sizeof(counters)))
            return;
        auto &peak = phase.load() == 0 ? enrollment : matching;
        size_t previous = peak.load();
        while (counters.WorkingSetSize > previous &&
               !peak.compare_exchange_weak(previous, counters.WorkingSetSize))
        {
        }
    }
    MemorySampler()
    {
        sample();
        try
        {
            thread = std::thread(
                [this]
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    while (
                        !wake.wait_for(lock, std::chrono::milliseconds(5), [this] { return done; }))
                    {
                        lock.unlock();
                        sample();
                        lock.lock();
                    }
                });
        }
        catch (const std::system_error &)
        { /* Boundary samples are still available. */
        }
    }
    void stop()
    {
        sample();
        {
            std::lock_guard<std::mutex> lock(mutex);
            done = true;
        }
        wake.notify_all();
        if (thread.joinable())
            thread.join();
    }
    ~MemorySampler() { stop(); }
};
} // namespace

EvaluationTiming evaluate(const Plan &plan, std::atomic<bool> &cancel,
                          const std::function<void(size_t, const Result &)> &onResult,
                          const std::function<void(size_t, int)> &onStarted)
{
    using Clock = std::chrono::steady_clock;
    auto started = Clock::now();
    EvaluationTiming timing;
    auto elapsed = [](auto from, auto to)
    { return std::chrono::duration<double, std::milli>(to - from).count(); };
    if (plan.settings.parallelism < 1 || plan.settings.parallelism > 16)
        throw std::runtime_error("동시 실행 수는 1~16 사이여야 합니다.");
    if (cancel || plan.pairs.empty())
        return timing;
#ifdef FINGERPRINT_DYNAMIC_BACKEND
    Backend backend(plan.settings.algorithm);
    auto enrollCall = backend.enroll;
    auto attemptCall = backend.attempt;
    auto compareCall = backend.compare;
#else
    auto enrollCall = &KISA_Enroll;
    auto attemptCall = &KISA_Attempt;
    auto compareCall = &KISA_Compare;
#endif
    HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized))
        throw std::runtime_error("이미지 읽기 구성 요소를 초기화하지 못했습니다.");
    struct Cleanup
    {
        ~Cleanup() { CoUninitialize(); }
    } cleanup;
    MemorySampler memory;
    auto collectMemory = [&]
    {
        memory.stop();
        timing.enrollmentWorkingSet = memory.enrollment;
        timing.matchingWorkingSet = memory.matching;
    };
    constexpr int capacity = 1000000;
    struct ImageState
    {
        FingerprintImage image;
        std::wstring error;
    };
    struct Template
    {
        std::vector<unsigned char> data;
        std::wstring error;
    };
    struct CachedAttempt
    {
        std::once_flag once;
        Template value;
        double milliseconds = 0;
    };
    std::map<size_t, ImageState> images;
    std::map<size_t, Template> enrollments;
    std::map<size_t, std::unique_ptr<CachedAttempt>> attempts;
    for (const auto &pair : plan.pairs)
    {
        if (cancel)
        {
            collectMemory();
            timing.totalMilliseconds = timing.preparationMilliseconds =
                elapsed(started, Clock::now());
            return timing;
        }
        if (pair.registration >= plan.images.size() || pair.attempt >= plan.images.size() ||
            pair.registration == pair.attempt)
            throw std::runtime_error("비교 목록의 이미지 참조를 확인하세요.");
        for (size_t index : {pair.registration, pair.attempt})
        {
            auto [it, inserted] = images.try_emplace(index);
            if (inserted)
                loadFingerprintImage(plan.images[index].path.c_str(), it->second.image,
                                     it->second.error);
        }
        if (plan.settings.reuseFeatures && !attempts.count(pair.attempt))
            attempts.emplace(pair.attempt, std::make_unique<CachedAttempt>());
        auto [it, inserted] = enrollments.try_emplace(pair.registration);
        if (!inserted)
            continue;
        ++timing.registrationsProcessed;
        auto &registration = it->second;
        const auto &input = images.at(pair.registration);
        if (!input.error.empty())
            registration.error = input.error;
        else
        {
            auto pixels = input.image.pixels;
            registration.data.resize(capacity);
            int bytes = 0, error = 0;
            char message[256]{};
            auto begin = Clock::now();
            int success = enrollCall(input.image.type, pixels.data(), registration.data.data(),
                                     &bytes, &error, message);
            timing.enrollmentMilliseconds += elapsed(begin, Clock::now());
            ++timing.enrollmentCalls;
            if (success != 1 || error != 0)
                registration.error = apiError(L"등록", error, message);
            else if (bytes <= 0 || bytes > capacity)
                registration.error = L"DLL의 템플릿 크기가 올바르지 않습니다.";
            else
            {
                registration.data.resize(bytes);
                ++timing.enrollmentTemplates;
                timing.templateBytes += bytes;
                timing.maxTemplateBytes =
                    (std::max)(timing.maxTemplateBytes, static_cast<size_t>(bytes));
            }
        }
        if (!registration.error.empty())
        {
            registration.data.clear();
            ++timing.registrationsRejected;
        }
        memory.sample();
    }
    auto matchingStarted = Clock::now();
    timing.preparationMilliseconds = elapsed(started, matchingStarted);
    memory.phase = 1;
    memory.sample();
    const auto &imageCache = images;
    const auto &templateCache = enrollments;
    const auto &attemptCache = attempts;
    auto extractAttempt = [&](size_t index, Template &output, double &milliseconds)
    {
        const auto &input = imageCache.at(index);
        auto pixels = input.image.pixels;
        output.data.resize(capacity);
        int bytes = 0, error = 0;
        char message[256]{};
        auto begin = Clock::now();
        int success = attemptCall(input.image.type, pixels.data(), output.data.data(), &bytes,
                                  &error, message);
        milliseconds = elapsed(begin, Clock::now());
        if (success != 1 || error != 0)
            output.error = apiError(L"비교 특징 추출", error, message);
        else if (bytes <= 0 || bytes > capacity)
            output.error = L"DLL의 비교 템플릿 크기가 올바르지 않습니다.";
        else
            output.data.resize(bytes);
        if (!output.error.empty())
            output.data.clear();
    };
    auto compare = [&](size_t row)
    {
        auto begin = Clock::now();
        Result result;
        auto finish = [&]
        {
            result.milliseconds = elapsed(begin, Clock::now());
            return result;
        };
        const auto &pair = plan.pairs[row];
        const auto &registration = templateCache.at(pair.registration);
        if (!registration.error.empty())
        {
            result.status = Status::EnrollmentError;
            result.detail = registration.error;
            return finish();
        }
        if (!imageCache.at(pair.attempt).error.empty())
        {
            result.status = Status::ImageError;
            result.detail = imageCache.at(pair.attempt).error;
            return finish();
        }
        Template local;
        const Template *probe;
        if (plan.settings.reuseFeatures)
        {
            auto &cached = *attemptCache.at(pair.attempt);
            bool produced = false;
            std::call_once(cached.once,
                           [&]
                           {
                               extractAttempt(pair.attempt, cached.value, cached.milliseconds);
                               produced = true;
                           });
            result.featureCalled = produced;
            if (produced)
                result.featureMilliseconds = cached.milliseconds;
            result.cacheHit = !produced && cached.value.error.empty();
            probe = &cached.value;
        }
        else
        {
            result.featureCalled = true;
            extractAttempt(pair.attempt, local, result.featureMilliseconds);
            probe = &local;
        }
        if (!probe->error.empty())
        {
            result.status = Status::AttemptError;
            result.detail = probe->error;
            return finish();
        }
        // Every mutable DLL call receives private copies, never cached template storage.
        auto enrolled = registration.data, attempted = probe->data;
        enrolled.resize(capacity);
        attempted.resize(capacity);
        int error = 0;
        char message[256]{};
        result.score = NAN;
        auto matchStarted = Clock::now();
        int success =
            compareCall(enrolled.data(), attempted.data(), &result.score, &error, message);
        result.matchMilliseconds = elapsed(matchStarted, Clock::now());
        if (success != 1 || error != 0)
        {
            result.status = Status::CompareError;
            result.detail = apiError(L"정합", error, message);
        }
        else if (!std::isfinite(result.score) || result.score < 0 || result.score > 1)
        {
            result.status = Status::CompareError;
            result.detail = L"점수가 0~1 범위의 유한한 값이 아닙니다.";
        }
        else
            result.status = Status::Success;
        return finish();
    };
    std::atomic<size_t> next{0};
    std::atomic<bool> stop{false};
    std::mutex delivery;
    std::exception_ptr error;
    size_t completionOrder = 0;
    auto worker = [&](int slot)
    {
        try
        {
            while (!cancel && !stop)
            {
                size_t row = next.fetch_add(1);
                if (row >= plan.pairs.size())
                    break;
                if (onStarted)
                {
                    std::lock_guard<std::mutex> guard(delivery);
                    onStarted(row, slot);
                }
                auto result = compare(row);
                std::lock_guard<std::mutex> guard(delivery);
                result.worker = slot;
                result.completionOrder = ++completionOrder;
                if (result.featureCalled)
                {
                    ++timing.attemptCalls;
                    timing.attemptMilliseconds += result.featureMilliseconds;
                }
                if (result.cacheHit)
                    ++timing.cacheHits;
                if (std::isfinite(result.matchMilliseconds))
                {
                    ++timing.compareCalls;
                    timing.compareMilliseconds += result.matchMilliseconds;
                }
                onResult(row, result);
            }
        }
        catch (...)
        {
            std::lock_guard<std::mutex> guard(delivery);
            if (!error)
                error = std::current_exception();
            stop = true;
        }
    };
    std::vector<std::thread> workers;
    int count = static_cast<int>(
        (std::min)(plan.pairs.size(), static_cast<size_t>(plan.settings.parallelism)));
    try
    {
        for (int i = 0; i < count; ++i)
            workers.emplace_back(worker, i + 1);
    }
    catch (...)
    {
        stop = true;
        for (auto &thread : workers)
            thread.join();
        throw;
    }
    for (auto &thread : workers)
        thread.join();
    if (error)
        std::rethrow_exception(error);
    collectMemory();
    auto ended = Clock::now();
    timing.matchingMilliseconds = elapsed(matchingStarted, ended);
    timing.totalMilliseconds = elapsed(started, ended);
    return timing;
}

Statistics summarize(const Plan &plan, const std::vector<Result> &results, double threshold)
{
    Statistics s;
    size_t failedGenuine = 0, matchCount = 0, genuineMatchCount = 0, impostorMatchCount = 0;
    double matchSum = 0, genuineMatchSum = 0, impostorMatchSum = 0;
    for (size_t i = 0; i < (std::min)(plan.pairs.size(), results.size()); ++i)
    {
        const auto &r = results[i];
        bool genuine = plan.pairs[i].genuine;
        if (r.status == Status::Pending || r.status == Status::Running ||
            r.status == Status::Cancelled)
            continue;
        if (std::isfinite(r.matchMilliseconds))
        {
            ++matchCount;
            matchSum += r.matchMilliseconds;
            if (genuine)
            {
                ++genuineMatchCount;
                genuineMatchSum += r.matchMilliseconds;
            }
            else
            {
                ++impostorMatchCount;
                impostorMatchSum += r.matchMilliseconds;
            }
        }
        ++s.completed;
        ++(genuine ? s.genuine : s.impostor);
        if (r.status != Status::Success)
        {
            ++s.failed;
            if (genuine)
                ++failedGenuine;
            continue;
        }
        (genuine ? s.genuineScores : s.impostorScores).push_back(r.score);
    }
    std::sort(s.genuineScores.begin(), s.genuineScores.end());
    std::sort(s.impostorScores.begin(), s.impostorScores.end());
    s.genuineOk = s.genuineScores.size();
    s.impostorOk = s.impostorScores.size();
    s.genuineRejected = s.genuine - s.genuineOk;
    s.impostorRejected = s.impostor - s.impostorOk;
    if (matchCount)
        s.matchMean = matchSum / matchCount;
    if (genuineMatchCount)
        s.genuineMatchMean = genuineMatchSum / genuineMatchCount;
    if (impostorMatchCount)
        s.impostorMatchMean = impostorMatchSum / impostorMatchCount;
    double sumG = 0, sumI = 0;
    for (double value : s.genuineScores)
        sumG += value;
    for (double value : s.impostorScores)
        sumI += value;
    if (s.genuineOk)
        s.genuineMean = sumG / s.genuineOk;
    if (s.impostorOk)
        s.impostorMean = sumI / s.impostorOk;
    auto point = [&](double t) -> Statistics::Point
    {
        size_t accepted = s.impostorScores.end() -
                          std::lower_bound(s.impostorScores.begin(), s.impostorScores.end(), t);
        size_t rejected = std::lower_bound(s.genuineScores.begin(), s.genuineScores.end(), t) -
                          s.genuineScores.begin();
        return {t, ratio(accepted, s.impostor), ratio(rejected + failedGenuine, s.genuine)};
    };
    auto current = point(threshold);
    s.fmr = current.fmr;
    s.fnmr = current.fnmr;
    std::set<double> thresholds{0, std::nextafter(1., 2.)};
    for (const auto &scores : {s.genuineScores, s.impostorScores})
        for (double value : scores)
        {
            thresholds.insert(value);
            thresholds.insert(std::nextafter(value, 2.));
        }
    for (double t : thresholds)
        s.curve.push_back(point(t));
    auto equalError = [](const std::vector<Statistics::Point> &curve)
    {
        for (size_t i = 0; i < curve.size(); ++i)
        {
            const auto &p = curve[i];
            if (!std::isfinite(p.fmr) || !std::isfinite(p.fnmr))
                continue;
            if (std::abs(p.fmr - p.fnmr) < 1e-12)
                return p.fmr;
            if (i && curve[i - 1].fnmr <= curve[i - 1].fmr && p.fnmr >= p.fmr)
                return (std::min)(curve[i - 1].fmr + curve[i - 1].fnmr, p.fmr + p.fnmr) / 2;
        }
        return std::numeric_limits<double>::quiet_NaN();
    };
    s.eer = equalError(s.curve);
    std::vector<Statistics::Point> successfulCurve;
    auto keepMinimum = [](double &target, double value)
    {
        if (!std::isfinite(target) || value < target)
            target = value;
    };
    for (const auto &p : s.curve)
    {
        size_t accepted =
            s.impostorScores.end() -
            std::lower_bound(s.impostorScores.begin(), s.impostorScores.end(), p.threshold);
        size_t rejected =
            std::lower_bound(s.genuineScores.begin(), s.genuineScores.end(), p.threshold) -
            s.genuineScores.begin();
        successfulCurve.push_back(
            {p.threshold, ratio(accepted, s.impostorOk), ratio(rejected, s.genuineOk)});
        if (!std::isfinite(p.fmr) || !std::isfinite(p.fnmr))
            continue;
        if (p.fmr <= .01 + 1e-12)
            keepMinimum(s.fmr100, p.fnmr);
        if (p.fmr <= .001 + 1e-12)
            keepMinimum(s.fmr1000, p.fnmr);
        if (p.fmr == 0)
            keepMinimum(s.zeroFmr, p.fnmr);
        if (p.fnmr == 0)
            keepMinimum(s.zeroFnmr, p.fmr);
    }
    s.eerWithoutFailures = equalError(successfulCurve);
    return s;
}
std::vector<EvaluationMetric> evaluationMetrics(const Statistics &s, const EvaluationTiming &t)
{
    auto fmt = [](double v, int digits, const wchar_t *unit)
    {
        if (!std::isfinite(v))
            return std::wstring(L"—");
        std::wostringstream out;
        out << std::fixed << std::setprecision(digits) << v << unit;
        return out.str();
    };
    auto pct = [&](double v) { return fmt(v * 100., 2, L"%"); };
    auto avg = [](double sum, size_t count) { return count ? sum / count : NAN; };
    std::vector<EvaluationMetric> out;
    auto add =
        [&](const wchar_t *group, const wchar_t *title, std::wstring value, std::wstring note)
    { out.push_back({group, title, std::move(value), std::move(note)}); };
    add(L"인식 성능", L"등오류율 · EER", pct(s.eer), L"처리 실패 포함 · 낮을수록 좋음");
    add(L"인식 성능", L"등오류율 · EER*", pct(s.eerWithoutFailures), L"성공한 비교만으로 계산");
    add(L"인식 성능", L"FMR100", pct(s.fmr100), L"FMR ≤ 1%에서 가장 낮은 FNMR");
    add(L"인식 성능", L"FMR1000", pct(s.fmr1000), L"FMR ≤ 0.1%에서 가장 낮은 FNMR");
    add(L"인식 성능", L"ZeroFMR", pct(s.zeroFmr), L"FMR = 0에서 가장 낮은 FNMR");
    add(L"인식 성능", L"ZeroFNMR",
        std::isfinite(s.zeroFnmr) ? pct(s.zeroFnmr)
        : s.genuineRejected       ? L"도달 불가"
                                  : L"—",
        L"FNMR = 0에서 가장 낮은 FMR");
    auto rejection = [&](const wchar_t *title, size_t n, size_t total)
    {
        add(L"처리 실패", title, pct(avg(static_cast<double>(n), total)),
            std::to_wstring(n) + L" / " + std::to_wstring(total));
    };
    rejection(L"등록 실패 · REJ ENROLL", t.registrationsRejected, t.registrationsProcessed);
    rejection(L"본인 비교 처리 실패 · REJ NGRA", s.genuineRejected, s.genuine);
    rejection(L"타인 비교 처리 실패 · REJ NIRA", s.impostorRejected, s.impostor);
    add(L"처리 시간", L"평균 등록 시간",
        fmt(avg(t.enrollmentMilliseconds, t.enrollmentCalls), 3, L" ms"), L"KISA_Enroll 호출 평균");
    add(L"처리 시간", L"평균 테스트 특징 추출 시간",
        fmt(avg(t.attemptMilliseconds, t.attemptCalls), 3, L" ms"), L"KISA_Attempt 실제 호출 평균");
    add(L"처리 시간", L"평균 정합 시간", fmt(s.matchMean, 3, L" ms"),
        L"KISA_Compare 호출 평균 · 특징 추출 제외");
    add(L"처리 시간", L"본인 비교 평균 정합 시간", fmt(s.genuineMatchMean, 3, L" ms"),
        L"같은 손가락 정합 API 평균");
    add(L"처리 시간", L"타인 비교 평균 정합 시간", fmt(s.impostorMatchMean, 3, L" ms"),
        L"다른 손가락 정합 API 평균");
    add(L"템플릿 · 메모리", L"평균 템플릿 크기",
        fmt(avg(static_cast<double>(t.templateBytes), t.enrollmentTemplates), 0, L" B"),
        L"성공한 등록 템플릿 평균");
    add(L"템플릿 · 메모리", L"최대 템플릿 크기",
        t.maxTemplateBytes ? fmt(static_cast<double>(t.maxTemplateBytes), 0, L" B") : L"—",
        L"성공한 등록 템플릿 최대");
    add(L"템플릿 · 메모리", L"등록 단계 앱 RAM 최대 관측",
        t.enrollmentWorkingSet ? fmt(t.enrollmentWorkingSet / (1024. * 1024.), 1, L" MiB") : L"—",
        L"앱 전체 Working Set · 5ms 간격 관측");
    add(L"템플릿 · 메모리", L"비교 단계 앱 RAM 최대 관측",
        t.matchingWorkingSet ? fmt(t.matchingWorkingSet / (1024. * 1024.), 1, L" MiB") : L"—",
        L"앱 전체 Working Set · 5ms 간격 관측");
    return out;
}

void saveEvaluationReport(const Plan &plan, const std::vector<Result> &results,
                          const EvaluationTiming &timing, double threshold,
                          double elapsedMilliseconds, bool interrupted, const fs::path &file)
{
    if (results.size() != plan.pairs.size())
        throw std::runtime_error("비교 결과와 목록의 크기가 다릅니다.");
    const auto stats = summarize(plan, results, threshold);
    if (!stats.completed)
        throw std::runtime_error("보고서로 저장할 완료된 비교가 없습니다.");
    auto escape = [](const std::wstring &value)
    {
        std::wstring out;
        for (wchar_t c : value)
        {
            switch (c)
            {
            case L'&':
                out += L"&amp;";
                break;
            case L'<':
                out += L"&lt;";
                break;
            case L'>':
                out += L"&gt;";
                break;
            case L'\"':
                out += L"&quot;";
                break;
            case L'\'':
                out += L"&#39;";
                break;
            default:
                out += c;
            }
        }
        return out;
    };
    auto fmt = [](double value, int digits = 3)
    {
        if (!std::isfinite(value))
            return std::wstring(L"—");
        std::wostringstream out;
        out.imbue(std::locale::classic());
        out << std::fixed << std::setprecision(digits) << value;
        return out.str();
    };
    auto pct = [&](double v) { return std::isfinite(v) ? fmt(v * 100, 2) + L"%" : L"—"; };
    std::wostringstream html;
    html.imbue(std::locale::classic());
    html
        << LR"REPORT(<!doctype html><html lang="ko"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>지문 인식 평가 보고서</title><style>
*{box-sizing:border-box}body{margin:0;background:#f4f6fb;color:#1d263e;font:15px/1.6 'Malgun Gothic',sans-serif}main{max-width:1180px;margin:36px auto;padding:32px}header{border-bottom:3px solid #5b4fe0;padding-bottom:24px}h1{font-size:34px;margin:8px 0}h2{font-size:22px;margin:32px 0 16px}h3{margin:0 0 14px;font-size:17px}.muted,small{color:#738099}.badge{display:inline-block;background:#eeebff;color:#5b4fe0;padding:5px 12px;border-radius:20px}.grid{display:grid;grid-template-columns:repeat(3,1fr);gap:16px}.card,section.panel{background:white;border:1px solid #e4e8f2;border-radius:16px;padding:20px}.value{font-size:32px;font-weight:700;color:#5b4fe0;margin:10px 0}table{width:100%;border-collapse:collapse;background:white}td,th{padding:11px 14px;text-align:left;border-bottom:1px solid #e4e8f2}th{background:#f0eefb}td.path{word-break:break-all}svg{width:100%;height:auto}svg text{font:11px 'Malgun Gothic',sans-serif;fill:#738099}.notice{padding:16px 20px;background:#eeebff;border-radius:12px;margin-top:20px}details{margin:24px 0}button{float:right;background:#5b4fe0;color:white;border:0;border-radius:10px;padding:12px 20px;cursor:pointer}.scroll{overflow:auto}footer{font-size:13px;color:#738099;margin-top:32px}@media(max-width:800px){main{padding:16px}.grid{grid-template-columns:1fr}}@media print{body{background:white}main{margin:0;padding:0}button{display:none}.card,section,table{break-inside:avoid}details{display:block}h2{break-after:avoid}}
</style><main><header><button onclick="document.querySelectorAll('details').forEach(d=>d.open=true);window.print()">인쇄 / PDF 저장</button><span class="badge">Fingerprint Lab · Evaluation</span><h1>지문 인식 평가 보고서</h1>)REPORT";
    SYSTEMTIME now{};
    GetLocalTime(&now);
    html << L"<p class=muted>생성 시각 (이 PC 현지 시간): " << now.wYear << L"-" << now.wMonth
         << L"-" << now.wDay << L" " << now.wHour << L":" << std::setw(2) << std::setfill(L'0')
         << now.wMinute << L"</p></header>";
    html << L"<div class=notice>"
         << (interrupted || stats.completed < plan.pairs.size() ? L"부분 평가 결과"
                                                                : L"전체 목록 평가 완료")
         << L" · 완료 " << stats.completed << L" / " << plan.pairs.size() << L"쌍 · 성공 "
         << stats.genuineOk + stats.impostorOk << L"쌍 · 처리 실패 " << stats.failed << L"쌍</div>";
    html << L"<h2>01 실험 정보</h2><table>";
    auto row = [&](const std::wstring &key, const std::wstring &value)
    {
        html << L"<tr><th>" << escape(key) << L"</th><td class=path>" << escape(value)
             << L"</td></tr>";
    };
    row(L"알고리즘 DLL", plan.settings.algorithm.wstring());
    row(L"테스트 DB", plan.settings.database.wstring());
    row(L"비교 목록", plan.sourceFile.empty() ? L"규칙으로 생성" : plan.sourceFile.wstring());
    row(L"목록 구성", L"이미지 " + std::to_wstring(plan.images.size()) + L"장 · 손가락 " +
                          std::to_wstring(plan.fingers) + L"개 · 등록 " +
                          std::to_wstring(plan.registrations) + L"장");
    const wchar_t *registration[]{L"첫 번째 촬영본", L"무작위 1장", L"촬영번호 지정",
                                  L"모든 촬영본 각각"};
    const wchar_t *comparison[]{L"모든 후보", L"첫 후보 1장", L"무작위 N개", L"촬영번호 지정"};
    auto rule = [](const wchar_t *const *names, int index)
    { return index >= 0 && index < 4 ? std::wstring(names[index]) : std::wstring(L"알 수 없음"); };
    row(L"등록 규칙", rule(registration, static_cast<int>(plan.settings.registration)) +
                          L" · 지정 촬영번호 " + std::to_wstring(plan.settings.registrationIndex));
    row(L"본인 / 타인 비교 규칙", rule(comparison, static_cast<int>(plan.settings.genuine)) +
                                      L" / " +
                                      rule(comparison, static_cast<int>(plan.settings.impostor)));
    row(L"재현 설정", L"본인 지정번호 " + std::to_wstring(plan.settings.genuineIndex) +
                          L" · 타인 지정번호 " + std::to_wstring(plan.settings.impostorIndex) +
                          L" · 랜덤 N=" + std::to_wstring(plan.settings.randomCount) + L" · 시드=" +
                          std::to_wstring(plan.settings.seed));
    row(L"실행 설정",
        L"동시 비교 " + std::to_wstring(plan.settings.parallelism) + L"개 · " +
            (plan.settings.reuseFeatures ? L"테스트 특징 재사용" : L"매번 특징 추출"));
    row(L"현재 판정 기준", fmt(threshold) + L" 이상이면 같은 지문으로 판정");
    row(L"전체 경과 시간 / 성공 처리량",
        fmt(elapsedMilliseconds / 1000.) + L" 초 / " +
            (elapsedMilliseconds > 0
                 ? fmt((stats.genuineOk + stats.impostorOk) * 1000. / elapsedMilliseconds, 2)
                 : L"—") +
            L" 쌍/초");
    row(L"준비 / 비교 단계 경과 시간", fmt(timing.preparationMilliseconds / 1000.) + L" 초 / " +
                                           fmt(timing.matchingMilliseconds / 1000.) + L" 초");
    row(L"API 호출 / 재사용", L"등록 " + std::to_wstring(timing.enrollmentCalls) +
                                  L" · 특징 추출 " + std::to_wstring(timing.attemptCalls) +
                                  L" · 정합 " + std::to_wstring(timing.compareCalls) +
                                  L" · 재사용 " + std::to_wstring(timing.cacheHits));
    html << L"</table><h2>02 인식 성능</h2><div class=grid>";
    const auto metrics = evaluationMetrics(stats, timing);
    for (size_t i = 0; i < 6; ++i)
        html << L"<div class=card><h3>" << escape(metrics[i].title) << L"</h3><div class=value>"
             << escape(metrics[i].value) << L"</div><small>" << escape(metrics[i].note)
             << L"</small></div>";
    html << L"</div><p>현재 기준의 다른 지문 오인식 비율 (FMR): <b>" << pct(stats.fmr)
         << L"</b> · 동일 지문 미인식 비율 (FNMR): <b>" << pct(stats.fnmr) << L"</b></p>";
    for (const auto *group : {L"처리 실패", L"처리 시간", L"템플릿 · 메모리"})
    {
        html << L"<h2>" << group
             << L"</h2><table><tr><th>지표</th><th>측정값</th><th>기준</th></tr>";
        for (const auto &m : metrics)
            if (m.group == group)
                html << L"<tr><td>" << escape(m.title) << L"</td><td><b>" << escape(m.value)
                     << L"</b></td><td>" << escape(m.note) << L"</td></tr>";
        html << L"</table>";
    }
    html << L"<h2>03 점수와 오류 곡선</h2><div class=grid>";
    for (int chart = 0; chart < 3; ++chart)
    {
        html << L"<section class=panel><h3>"
             << (chart == 0   ? L"점수 분포"
                 : chart == 1 ? L"FMR(t) / FNMR(t)"
                              : L"ROC · FMR / FNMR (로그축)")
             << L"</h3><svg viewBox='0 0 360 250' role='img'>";
        for (int g = 0; g <= 4; ++g)
        {
            int y = 210 - g * 45;
            html << L"<path d='M40 " << y << L" H340' stroke='#e4e8f2'/><text x='2' y='" << y + 4
                 << L"'>"
                 << (chart == 2 ? fmt(std::pow(10., g - 2), 2) + L"%"
                                : std::to_wstring(g * 25) + L"%")
                 << L"</text>";
        }
        if (chart == 0)
        {
            std::array<size_t, 20> genuine{}, impostor{};
            for (double v : stats.genuineScores)
                ++genuine[(std::min)(19, static_cast<int>(v * 20))];
            for (double v : stats.impostorScores)
                ++impostor[(std::min)(19, static_cast<int>(v * 20))];
            for (int b = 0; b < 20; ++b)
                for (int k = 0; k < 2; ++k)
                {
                    double count = k ? impostor[b] : genuine[b],
                           total = k ? stats.impostorOk : stats.genuineOk;
                    double h = total ? 180 * count / total : 0;
                    html << L"<rect x='" << 40 + b * 15 + k * 7 << L"' y='" << fmt(210 - h)
                         << L"' width='6' height='" << fmt(h) << L"' fill='"
                         << (k ? L"#d5912b" : L"#1a8f76") << L"'/>";
                }
        }
        else
            for (int series = 0; series < (chart == 1 ? 2 : 1); ++series)
            {
                html << L"<polyline fill='none' stroke='"
                     << (chart == 2 ? L"#5b4fe0"
                         : series   ? L"#1a8f76"
                                    : L"#d5912b")
                     << L"' stroke-width='2' points='";
                for (const auto &p : stats.curve)
                {
                    double x = chart == 1 ? (std::min)(1., p.threshold) : p.fmr,
                           y = chart == 1 ? (series ? p.fnmr : p.fmr) : p.fnmr;
                    if (!std::isfinite(x) || !std::isfinite(y))
                        continue;
                    if (chart == 2)
                    {
                        x = (std::log10((std::max)(1e-4, x)) + 4) / 4;
                        y = (std::log10((std::max)(1e-4, y)) + 4) / 4;
                    }
                    html << fmt(40 + 300 * x) << L"," << fmt(210 - 180 * y) << L" ";
                }
                html << L"'/>";
            }
        for (int tick = 0; tick <= 4; ++tick)
            html << L"<text text-anchor='middle' x='" << 40 + 75 * tick << L"' y='229'>"
                 << (chart == 2 ? fmt(std::pow(10., tick - 2), 2) + L"%" : fmt(tick / 4., 2))
                 << L"</text>";
        html << L"</svg>";
        html << L"<small>"
             << (chart == 0
                     ? L"초록: 같은 손가락 · 주황: 다른 손가락. 각 집단의 성공 비교 수로 정규화."
                 : chart == 1 ? L"가로: 판정 기준 · 주황: FMR · 초록: FNMR"
                              : L"가로: FMR · 세로: FNMR. 0은 0.01% 눈금에 표시.")
             << L"</small></section>";
    }
    html << L"</div><h2>04 비교별 결과</h2><p class=muted>원래 실행 목록 순서입니다. 처리 실패와 "
            L"미완료 행도 보존합니다.</p><details><summary>전체 "
         << plan.pairs.size()
         << L"쌍 펼쳐 보기</summary><div class=scroll><table><tr><th>#</th><th>등록 "
            L"파일</th><th>비교 파일</th><th>실제 관계</th><th>점수</th><th>판정 / 처리 "
            L"상태</th><th>지연(ms)</th><th>작업</th><th>상세</th></tr>";
    for (size_t i = 0; i < plan.pairs.size(); ++i)
    {
        const auto &p = plan.pairs[i];
        const auto &r = results[i];
        std::wstring status;
        switch (r.status)
        {
        case Status::Success:
            status = r.score >= threshold ? (p.genuine ? L"동일 지문 인식" : L"다른 지문 오인식")
                                          : (p.genuine ? L"동일 지문 미인식" : L"다른 지문 구분");
            break;
        case Status::EnrollmentError:
            status = L"등록 실패";
            break;
        case Status::AttemptError:
            status = L"특징 추출 실패";
            break;
        case Status::CompareError:
            status = L"정합 실패";
            break;
        case Status::ImageError:
            status = L"이미지 읽기 실패";
            break;
        case Status::Cancelled:
            status = L"취소";
            break;
        default:
            status = L"미완료";
        }
        html << L"<tr><td>" << i + 1 << L"</td><td>"
             << escape(plan.images.at(p.registration).path.filename().wstring()) << L"</td><td>"
             << escape(plan.images.at(p.attempt).path.filename().wstring()) << L"</td><td>"
             << (p.genuine ? L"같은 손가락" : L"다른 손가락") << L"</td><td>"
             << (r.status == Status::Success ? fmt(r.score) : L"—") << L"</td><td>" << status
             << L"</td><td>"
             << (r.status == Status::Pending || r.status == Status::Cancelled ? L"—"
                                                                              : fmt(r.milliseconds))
             << L"</td><td>" << r.worker << L"</td><td>" << escape(r.detail) << L"</td></tr>";
    }
    html << L"</table></div></details><footer><h3>해석과 측정 범위</h3><p>선택한 DB와 비교 "
            L"목록에서 얻은 결과이며 공식 FVC 순위와 직접 비교할 수 없습니다. 정합 점수는 동일인 "
            L"확률이 아닙니다. 본인 비교는 같은 손가락의 다른 촬영본, 타인 비교는 다른 "
            L"손가락입니다.</p><p>FNMR은 본인 비교의 처리 실패를 포함합니다. EER은 관측 임계값의 "
            L"오류율 교차 지점에 인접한 값 중 최소 평균으로 추정합니다. EER*은 실패를 제외합니다. "
            L"—는 계산할 표본 또는 측정값이 없음을 뜻합니다.</p><p>타인 비교 1건의 오류율 해상도: "
         << pct(stats.impostor ? 1. / stats.impostor : NAN)
         << L". 작은 표본에서 0.1% 성능을 보장할 수 없습니다.</p><p>등록 시간은 첫 모델 초기화를 "
            L"포함할 수 있습니다. 정합 시간은 특징 추출을 제외한 API 호출 시간이며 병렬 실행 "
            L"시간의 합과 전체 경과 시간은 다릅니다. RAM은 DLL 단독이 아닌 앱·모델·캐시 전체의 "
            L"Working Set을 5ms 간격으로 관측한 값입니다.</p></footer></main></html>";
    const auto wide = html.str();
    int bytes = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr,
                                    0, nullptr, nullptr);
    if (!bytes)
        throw std::runtime_error("보고서 인코딩에 실패했습니다.");
    std::string utf8(bytes, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), bytes,
                        nullptr, nullptr);
    std::ofstream out(file, std::ios::binary);
    out.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    out.close();
    if (!out)
        throw std::runtime_error(
            "보고서를 저장하지 못했습니다. 저장 위치와 쓰기 권한을 확인하세요.");
}

} // namespace demo
