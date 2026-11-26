#include "feature_extractor.h"
#include "deepprint_preprocessing.h"
#include "deepprint_resource.h"

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
constexpr wchar_t kModuleAnchor[] = L"DeepPrint";
constexpr std::array<int64_t, 4> kInputShape{1, 1, kDeepPrintInputSize, kDeepPrintInputSize};

HMODULE recognitionModule(DWORD flags)
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | flags, kModuleAnchor, &module)) {
        throw std::runtime_error("Unable to locate the fingerprint DLL");
    }
    return module;
}

fs::path recognitionDirectory(HMODULE module)
{
    std::wstring path(32768, L'\0');
    const auto size = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
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
    explicit DeepPrintRuntime(HMODULE module)
        : library_(loadRuntime(recognitionDirectory(module))), environment_(ORT_LOGGING_LEVEL_WARNING, "DeepPrint"),
          memory_(Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault))
    {
        const auto resource = FindResource(module, TEXT(DEEPPRINT_MODEL_RESOURCE_NAME), RT_RCDATA);
        const auto handle = resource ? LoadResource(module, resource) : nullptr;
        const auto data = handle ? LockResource(handle) : nullptr;
        const auto size = resource ? SizeofResource(module, resource) : 0;
        if (!data || !size) {
            throw std::runtime_error("Cannot read the embedded DeepPrint model");
        }
        Ort::SessionOptions options;
        options.SetIntraOpNumThreads(4);
        session_ = Ort::Session(environment_, data, size, options);
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
        auto session = std::make_unique<DeepPrintRuntime>(recognitionModule(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT));
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
    auto input = preprocessDeepPrint(image, format, deepPrintPreprocessingForType(iType, FINGERPRINT_FVC2004_ROI));
    runtime().extract(input, features);
    return true;
}

} // namespace fingerprint
