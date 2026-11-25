#include "feature_extractor.h"
#include "deepprint_preprocessing.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#define ORT_API_MANUAL_INIT
#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace fingerprint {
namespace {

namespace fs = std::filesystem;
constexpr wchar_t kModelFile[] = L"deepprint_texminu_512.onnx";
constexpr std::array<int64_t, 4> kInputShape{1, 1, kDeepPrintInputSize, kDeepPrintInputSize};

HMODULE recognitionModule(DWORD flags)
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | flags, kModelFile, &module)) {
        throw std::runtime_error("Unable to locate the fingerprint DLL");
    }
    return module;
}

fs::path recognitionDirectory()
{
    std::wstring path(32768, L'\0');
    const auto size = GetModuleFileNameW(recognitionModule(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT),
                                         path.data(), static_cast<DWORD>(path.size()));
    if (size == 0 || size >= path.size()) {
        throw std::runtime_error("Unable to read the fingerprint DLL path");
    }
    path.resize(size);
    return fs::path(path).parent_path();
}

struct ReleaseLibrary {
    void operator()(HMODULE module) const { FreeLibrary(module); }
};
using Library = std::unique_ptr<std::remove_pointer_t<HMODULE>, ReleaseLibrary>;

Library loadRuntime(const fs::path& directory)
{
    Library library(LoadLibraryExW((directory / L"onnxruntime.dll").c_str(), nullptr,
                                   LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32));
    if (!library) {
        const auto error = GetLastError();
        throw std::runtime_error("Cannot load onnxruntime.dll beside IndustryAlgorithm.dll (Windows error "
                                 + std::to_string(error) + ")");
    }
    const auto symbol = GetProcAddress(library.get(), "OrtGetApiBase");
    if (!symbol) {
        throw std::runtime_error("onnxruntime.dll does not export OrtGetApiBase");
    }
    decltype(&OrtGetApiBase) getApiBase = nullptr;
    static_assert(sizeof(getApiBase) == sizeof(symbol));
    std::memcpy(&getApiBase, &symbol, sizeof(symbol));
    const OrtApi* api = getApiBase()->GetApi(ORT_API_VERSION);
    if (!api) {
        throw std::runtime_error("Incompatible ONNX Runtime API version");
    }
    Ort::InitApi(api);
    return library;
}

class DeepPrintRuntime {
public:
    explicit DeepPrintRuntime(const fs::path& directory)
        : library_(loadRuntime(directory)), environment_(ORT_LOGGING_LEVEL_WARNING, "DeepPrint"),
          memory_(Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault))
    {
        const auto modelPath = directory / kModelFile;
        if (!fs::is_regular_file(modelPath)) {
            throw std::runtime_error("Missing deepprint_texminu_512.onnx beside IndustryAlgorithm.dll");
        }
        Ort::SessionOptions options;
        options.SetIntraOpNumThreads(4);
        session_ = Ort::Session(environment_, modelPath.c_str(), options);
    }

    void extract(std::vector<float>& image, Features& embedding)
    {
        auto input = Ort::Value::CreateTensor<float>(memory_, image.data(), image.size(),
                                                      kInputShape.data(), kInputShape.size());
        const char* inputName = "image";
        const char* outputName = "embedding";
        auto output = session_.Run(Ort::RunOptions{nullptr}, &inputName, &input, 1, &outputName, 1);
        const auto info = output.front().GetTensorTypeAndShapeInfo();
        const auto shape = info.GetShape();
        if (info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT
            || shape.size() != 2 || shape[0] != 1
            || shape[1] != static_cast<std::int64_t>(kFeatureCount)) {
            throw std::runtime_error("DeepPrint model must produce one 512-float embedding");
        }
        const auto* values = output.front().GetTensorData<float>();
        std::copy_n(values, kFeatureCount, embedding.begin());
        double squaredNorm = 0.0;
        for (const float value : embedding) {
            if (!std::isfinite(value) || std::abs(value) > 1.001f) {
                throw std::runtime_error("DeepPrint model produced an invalid embedding");
            }
            squaredNorm += static_cast<double>(value) * value;
        }
        if (squaredNorm == 0.0) {
            throw std::runtime_error("DeepPrint model produced a zero embedding");
        }
    }

private:
    Library library_;
    Ort::Env environment_;
    Ort::MemoryInfo memory_;
    Ort::Session session_{nullptr};
};

DeepPrintRuntime& runtime()
{
    // KISA exposes no teardown entry point. Keep the inference session and its
    // worker pool resident until process exit, outside Windows DLL detach.
    static DeepPrintRuntime* const instance = [] {
        auto session = std::make_unique<DeepPrintRuntime>(recognitionDirectory());
        recognitionModule(GET_MODULE_HANDLE_EX_FLAG_PIN);
        return session.release();
    }();
    return *instance;
}

} // namespace

bool extractFeatures(int iType, const unsigned char* image, Features& features)
{
    const auto format = imageFormat(iType);
    if (format.width == 0) {
        return false;
    }
    auto input = preprocessDeepPrint(image, format);
    runtime().extract(input, features);
    return true;
}

} // namespace fingerprint
