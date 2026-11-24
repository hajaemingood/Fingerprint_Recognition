#include "tiff_image.h"
#include "image_format.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>

#include <memory>
#include <stdexcept>
#include <string>

namespace fvc {
namespace {

void check(HRESULT result, const char* operation)
{
    if (FAILED(result)) {
        throw std::runtime_error(std::string(operation) + " failed (HRESULT "
                                 + std::to_string(result) + ")");
    }
}

struct ComApartment {
    ComApartment() { check(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED), "Initialize COM"); }
    ~ComApartment() { CoUninitialize(); }
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;
};

struct ReleaseCom {
    void operator()(IUnknown* object) const { object->Release(); }
};

template <class T>
using ComPtr = std::unique_ptr<T, ReleaseCom>;

template <class T, class Create>
ComPtr<T> createCom(Create create, const char* operation)
{
    T* result = nullptr;
    check(create(&result), operation);
    return ComPtr<T>(result);
}

ComPtr<IWICImagingFactory> makeFactory()
{
    return createCom<IWICImagingFactory>([](auto** result) {
        return CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(result));
    }, "Create WIC factory");
}

int findType(UINT width, UINT height)
{
    for (const int type : {1, 2, 3}) {
        const auto format = fingerprint::imageFormat(type);
        if (width == static_cast<UINT>(format.width) && height == static_cast<UINT>(format.height)) {
            return type;
        }
    }
    throw std::runtime_error("Unsupported FVC image size "
                             + std::to_string(width) + "x" + std::to_string(height));
}

} // namespace

GrayImage readTiff(const std::filesystem::path& path)
{
    try {
        const ComApartment com;
        const auto factory = makeFactory();
        const auto decoder = createCom<IWICBitmapDecoder>([&](auto** result) {
            return factory->CreateDecoderFromFilename(
                path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, result);
        }, "Open TIFF");
        GUID container{};
        check(decoder->GetContainerFormat(&container), "Read TIFF container");
        if (container != GUID_ContainerFormatTiff) {
            throw std::runtime_error("Expected a TIFF file");
        }

        const auto frame = createCom<IWICBitmapFrameDecode>([&](auto** result) {
            return decoder->GetFrame(0, result);
        }, "Read TIFF frame");
        UINT width = 0;
        UINT height = 0;
        check(frame->GetSize(&width, &height), "Read TIFF dimensions");
        GrayImage image{findType(width, height), width, height,
                        std::vector<unsigned char>(width * height)};
        const auto converter = createCom<IWICFormatConverter>([&](auto** result) {
            return factory->CreateFormatConverter(result);
        }, "Create TIFF pixel converter");
        check(converter->Initialize(frame.get(), GUID_WICPixelFormat8bppGray,
                                    WICBitmapDitherTypeNone, nullptr, 0.0,
                                    WICBitmapPaletteTypeCustom), "Convert to 8-bit grayscale");
        check(converter->CopyPixels(nullptr, width, width * height, image.pixels.data()),
              "Read TIFF pixels");
        return image;
    } catch (const std::exception& error) {
        throw std::runtime_error(path.u8string() + ": " + error.what());
    }
}

void writeTiff(const std::filesystem::path& path, const GrayImage& image)
{
    try {
        if (findType(image.width, image.height) != image.type
            || image.pixels.size() != image.width * image.height) {
            throw std::runtime_error("Inconsistent FVC image dimensions or pixel count");
        }

        const ComApartment com;
        const auto factory = makeFactory();
        const auto stream = createCom<IWICStream>([&](auto** result) {
            return factory->CreateStream(result);
        }, "Create TIFF stream");
        check(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE), "Open TIFF output");
        const auto encoder = createCom<IWICBitmapEncoder>([&](auto** result) {
            return factory->CreateEncoder(GUID_ContainerFormatTiff, nullptr, result);
        }, "Create TIFF encoder");
        check(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache), "Initialize TIFF encoder");
        IPropertyBag2* optionsRaw = nullptr;
        const auto frame = createCom<IWICBitmapFrameEncode>([&](auto** result) {
            return encoder->CreateNewFrame(result, &optionsRaw);
        }, "Create TIFF output frame");
        const ComPtr<IPropertyBag2> options(optionsRaw);
        if (options) {
            wchar_t name[] = L"TiffCompressionMethod";
            PROPBAG2 property{};
            property.pstrName = name;
            VARIANT value{};
            value.vt = VT_UI1;
            value.bVal = WICTiffCompressionLZW;
            check(options->Write(1, &property, &value), "Set lossless TIFF compression");
        }
        check(frame->Initialize(options.get()), "Initialize TIFF frame");
        check(frame->SetSize(image.width, image.height), "Set TIFF dimensions");
        WICPixelFormatGUID format = GUID_WICPixelFormat8bppGray;
        check(frame->SetPixelFormat(&format), "Set TIFF pixel format");
        if (format != GUID_WICPixelFormat8bppGray) {
            throw std::runtime_error("TIFF encoder did not accept 8-bit grayscale");
        }
        check(frame->WritePixels(image.height, image.width, static_cast<UINT>(image.pixels.size()),
                                 const_cast<unsigned char*>(image.pixels.data())), "Write TIFF pixels");
        check(frame->Commit(), "Finish TIFF frame");
        check(encoder->Commit(), "Finish TIFF file");
    } catch (const std::exception& error) {
        throw std::runtime_error(path.u8string() + ": " + error.what());
    }
}

} // namespace fvc
