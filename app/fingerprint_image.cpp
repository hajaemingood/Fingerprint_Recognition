#include "fingerprint_image.h"
#include "image_format.h"

#include <windows.h>
#include <wincodec.h>
#include <memory>
#include <utility>

namespace {

struct ReleaseCom {
    void operator()(IUnknown* object) const { object->Release(); }
};

template <class T>
using ComPtr = std::unique_ptr<T, ReleaseCom>;

bool fail(std::wstring& error, const wchar_t* message)
{
    error = message;
    return false;
}

} // namespace

bool loadFingerprintImage(const wchar_t* path, FingerprintImage& image, std::wstring& error)
{
    IWICImagingFactory* factoryRaw = nullptr;
    const HRESULT initialized = CoCreateInstance(
        CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factoryRaw));
    if (FAILED(initialized)) {
        return fail(error, L"Windows 이미지 디코더를 시작할 수 없습니다.");
    }
    const ComPtr<IWICImagingFactory> factory(factoryRaw);

    IWICBitmapDecoder* decoderRaw = nullptr;
    if (FAILED(factory->CreateDecoderFromFilename(
            path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoderRaw))) {
        return fail(error, L"이미지 파일을 열 수 없습니다.");
    }
    const ComPtr<IWICBitmapDecoder> decoder(decoderRaw);

    GUID container{};
    if (FAILED(decoder->GetContainerFormat(&container)) || container != GUID_ContainerFormatTiff) {
        return fail(error, L"TIFF(.tif, .tiff) 파일을 선택하세요.");
    }

    IWICBitmapFrameDecode* frameRaw = nullptr;
    if (FAILED(decoder->GetFrame(0, &frameRaw))) {
        return fail(error, L"TIFF의 첫 번째 영상을 읽을 수 없습니다.");
    }
    const ComPtr<IWICBitmapFrameDecode> frame(frameRaw);

    UINT width = 0;
    UINT height = 0;
    if (FAILED(frame->GetSize(&width, &height))) {
        return fail(error, L"영상 크기를 읽을 수 없습니다.");
    }
    FingerprintImage decoded;
    for (const int type : {1, 2, 3}) {
        const auto format = fingerprint::imageFormat(type);
        if (width == static_cast<UINT>(format.width) && height == static_cast<UINT>(format.height)) {
            decoded.type = type;
            break;
        }
    }
    if (decoded.type == 0) {
        return fail(error, L"지원 영상 크기는 448×478, 300×300, 640×480입니다.");
    }

    IWICFormatConverter* converterRaw = nullptr;
    if (FAILED(factory->CreateFormatConverter(&converterRaw))) {
        return fail(error, L"회색조 변환기를 시작할 수 없습니다.");
    }
    const ComPtr<IWICFormatConverter> converter(converterRaw);
    if (FAILED(converter->Initialize(frame.get(), GUID_WICPixelFormat8bppGray,
            WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
        return fail(error, L"영상을 8비트 회색조로 변환할 수 없습니다.");
    }

    decoded.pixels.resize(width * height);
    if (FAILED(converter->CopyPixels(nullptr, width, width * height, decoded.pixels.data()))) {
        return fail(error, L"TIFF 픽셀을 읽을 수 없습니다.");
    }
    image = std::move(decoded);
    error.clear();
    return true;
}
