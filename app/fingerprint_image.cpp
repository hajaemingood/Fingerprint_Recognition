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
#include <mutex>
#include <thread>
#include <exception>

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
    for (const int type : {1, 2, 3})
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
        return fail(error, L"지원 영상 크기는 448×478, 300×300, 640×480입니다.");
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
    out << "#database," << csvField(fs::absolute(s.database).u8string()) << "\r\n";
    out << "#settings," << static_cast<int>(s.registration) << ',' << static_cast<int>(s.genuine)
        << ',' << static_cast<int>(s.impostor) << ',' << s.registrationIndex << ','
        << s.genuineIndex << ',' << s.impostorIndex << ',' << s.randomCount << ',' << s.seed << ','
        << s.parallelism << "\r\n";
    out << "enroll,attempt,relation\r\n";
    // Save the actual directed pairs in display order, not just a random seed.
    for (const auto &p : plan.pairs)
    {
        out << csvField(plan.images.at(p.registration).path.u8string()) << ','
            << csvField(plan.images.at(p.attempt).path.u8string()) << ','
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
    fs::path actualDatabase;
    auto addImage = [&](const std::string &value)
    {
        if (value.empty())
            throw std::runtime_error("CSV에 빈 이미지 경로가 있습니다.");
        auto path = fs::u8path(value);
        if (!path.is_absolute())
            path = plan.settings.database / path;
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
                plan.settings.database = fs::weakly_canonical(
                    currentDatabasePath(path.is_absolute() ? path : file.parent_path() / path));
                if (!fs::is_directory(plan.settings.database))
                    throw std::runtime_error("CSV에 저장된 DB 폴더를 찾을 수 없습니다.");
                databaseMetadata = true;
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
struct Backend
{
    HMODULE library = nullptr;
    decltype(&KISA_Enroll) enroll = nullptr;
    decltype(&KISA_Attempt) attempt = nullptr;
    decltype(&KISA_Compare) compare = nullptr;
    Backend()
    {
        auto dll = backendDirectory() / L"IndustryAlgorithm.dll";
        library = LoadLibraryExW(dll.c_str(), nullptr,
                                 LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!library)
            throw std::runtime_error(
                "DeepPrint DLL을 읽을 수 없습니다. 실행 파일 옆에 IndustryAlgorithm.dll, "
                "deepprint_texminu_512.onnx, onnxruntime.dll을 준비하세요.");
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
bool deepprintTemplate(const std::vector<unsigned char> &data, int bytes)
{
    constexpr unsigned char header[]{'D', 'P', 'T', 'M', 1, 0, 0, 2};
    return bytes == 2056 && std::equal(std::begin(header), std::end(header), data.begin());
}
#endif
} // namespace
std::wstring recognitionBackendLabel()
{
    auto directory = backendDirectory();
    for (const auto *file :
         {L"IndustryAlgorithm.dll", L"deepprint_texminu_512.onnx", L"onnxruntime.dll"})
        if (!fs::is_regular_file(directory / file))
            return L"DeepPrint · 실행 파일 준비 필요";
    return L"DeepPrint · 질감 22% · 특징점 78%";
}

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
    Backend backend;
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
    std::map<size_t, ImageState> images;
    std::map<size_t, Template> enrollments;
    // Prepare immutable inputs and enroll each reference image exactly once.
    // This keeps serial and parallel evaluations on the same enrollment templates.
    for (const auto &pair : plan.pairs)
    {
        if (cancel)
        {
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
        auto [it, inserted] = enrollments.try_emplace(pair.registration);
        if (!inserted)
            continue;
        auto &registration = it->second;
        const auto &input = images.at(pair.registration);
        if (!input.error.empty())
        {
            registration.error = input.error;
            continue;
        }
        auto pixels = input.image.pixels;
        registration.data.resize(capacity);
        int bytes = 0, error = 0;
        char message[256]{};
        int success = enrollCall(input.image.type, pixels.data(), registration.data.data(), &bytes,
                                 &error, message);
        if (success != 1 || error != 0)
            registration.error = apiError(L"등록", error, message);
        else if (bytes <= 0 || bytes > capacity)
            registration.error = L"DLL의 템플릿 크기가 올바르지 않습니다.";
#ifdef FINGERPRINT_DYNAMIC_BACKEND
        else if (!deepprintTemplate(registration.data, bytes))
            registration.error = L"DeepPrint 형식의 템플릿이 아닙니다. 이전 히스토그램 DLL 대신 "
                                 L"최신 DeepPrint DLL을 사용하세요.";
#endif
        else
            registration.data.resize(bytes);
        if (!registration.error.empty())
            registration.data.clear();
    }
    auto matchingStarted = Clock::now();
    timing.preparationMilliseconds = elapsed(started, matchingStarted);
    // Workers only read these caches. Each API call gets private mutable buffers.
    const auto &imageCache = images;
    const auto &templateCache = enrollments;
    auto compare = [&](size_t row)
    {
        const auto &pair = plan.pairs[row];
        Result result;
        const auto &registration = templateCache.at(pair.registration);
        if (!registration.error.empty())
        {
            result.status = Status::EnrollmentError;
            result.detail = registration.error;
            return result;
        }
        const auto &input = imageCache.at(pair.attempt);
        if (!input.error.empty())
        {
            result.status = Status::ImageError;
            result.detail = input.error;
            return result;
        }
        auto start = Clock::now();
        auto pixels = input.image.pixels;
        std::vector<unsigned char> attempt(capacity), enrolled = registration.data;
        enrolled.resize(capacity);
        int bytes = 0, error = 0;
        char message[256]{};
        int success =
            attemptCall(input.image.type, pixels.data(), attempt.data(), &bytes, &error, message);
        if (success != 1 || error != 0 || bytes <= 0 || bytes > capacity
#ifdef FINGERPRINT_DYNAMIC_BACKEND
            || !deepprintTemplate(attempt, bytes)
#endif
        )
        {
            result.status = Status::AttemptError;
            result.detail = apiError(L"비교 특징 추출", error, message);
        }
        else
        {
            result.score = NAN;
            error = 0;
            std::fill(std::begin(message), std::end(message), '\0');
            success = compareCall(enrolled.data(), attempt.data(), &result.score, &error, message);
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
        }
        result.milliseconds =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count();
        return result;
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
                onResult(row, result); // Original row ID is independent of completion order.
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
    auto ended = Clock::now();
    timing.matchingMilliseconds = elapsed(matchingStarted, ended);
    timing.totalMilliseconds = elapsed(started, ended);
    return timing;
}
Statistics summarize(const Plan &plan, const std::vector<Result> &results, double threshold)
{
    Statistics s;
    size_t failedGenuine = 0;
    for (size_t i = 0; i < (std::min)(plan.pairs.size(), results.size()); ++i)
    {
        const auto &r = results[i];
        bool genuine = plan.pairs[i].genuine;
        if (r.status == Status::Pending || r.status == Status::Running ||
            r.status == Status::Cancelled)
            continue;
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
    if (s.genuine && s.impostor)
        for (size_t i = 0; i < s.curve.size(); ++i)
        {
            const auto &p = s.curve[i];
            if (std::abs(p.fmr - p.fnmr) < 1e-12)
            {
                s.eer = p.fmr;
                break;
            }
            if (i && s.curve[i - 1].fnmr <= s.curve[i - 1].fmr && p.fnmr >= p.fmr)
            {
                const auto &previous = s.curve[i - 1];
                s.eer = (std::min)(previous.fmr + previous.fnmr, p.fmr + p.fnmr) / 2;
                break;
            }
        }
    return s;
}
} // namespace demo
