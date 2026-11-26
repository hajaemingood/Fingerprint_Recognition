#pragma once

#include "image_format.h"

#include <vector>

namespace fingerprint {

inline constexpr int kDeepPrintInputSize = 299;

enum class DeepPrintPreprocessing {
    Standard,
    Bilateral,
    ForegroundCrop,
    ForegroundCenter
};

inline DeepPrintPreprocessing deepPrintPreprocessingForType(int iType, bool useFvc2004Roi)
{
    if (iType == 2) {
        return DeepPrintPreprocessing::Bilateral;
    }
    if (iType == 3 && useFvc2004Roi) {
        return DeepPrintPreprocessing::ForegroundCrop;
    }
    return DeepPrintPreprocessing::Standard;
}

std::vector<float> preprocessDeepPrint(
    const unsigned char* image, ImageFormat format,
    DeepPrintPreprocessing preprocessing = DeepPrintPreprocessing::Standard);

std::vector<float> preprocessDeepPrintVariant(const unsigned char* image, ImageFormat format,
    DeepPrintPreprocessing mode, int rangeSigma, int marginPercent, bool bicubic);

} // namespace fingerprint
