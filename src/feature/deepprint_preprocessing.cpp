#include "deepprint_preprocessing.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace fingerprint {
namespace {

struct ResizeWeights {
    int first = 0;
    std::vector<float> coefficients;
};

std::array<ResizeWeights, kDeepPrintInputSize> makeWeights(int side)
{
    std::array<ResizeWeights, kDeepPrintInputSize> weights;
    const float scale = static_cast<float>(side) / kDeepPrintInputSize;
    const float support = std::max(1.0f, scale);
    const float inverseSupport = 1.0f / support;
    for (int output = 0; output < kDeepPrintInputSize; ++output) {
        // Match the float32 pixel-center and weighting convention of flx.
        const float center = (output + 0.5f) * scale;
        auto& filter = weights[output];
        filter.first = std::max(0, static_cast<int>(std::ceil(center - support - 0.5f)));
        const int last = std::min(side - 1, static_cast<int>(std::floor(center + support - 0.5f)));
        float sum = 0.0f;
        for (int pixel = filter.first; pixel <= last; ++pixel) {
            const float distance = (pixel - center + 0.5f) * inverseSupport;
            const float value = std::max(0.0f, 1.0f - std::abs(distance));
            filter.coefficients.push_back(value);
            sum += value;
        }
        for (float& value : filter.coefficients) {
            value /= sum;
        }
    }
    return weights;
}

} // namespace

std::vector<float> preprocessDeepPrint(const unsigned char* image, ImageFormat format)
{
    const int side = std::max(format.width, format.height);
    const int left = (side - format.width) / 2;
    const int top = (side - format.height) / 2;
    const auto weights = makeWeights(side);
    std::vector<float> horizontal(side * kDeepPrintInputSize, 1.0f);

    for (int y = 0; y < format.height; ++y) {
        for (int x = 0; x < kDeepPrintInputSize; ++x) {
            const auto& filter = weights[x];
            float value = 0.0f;
            int column = filter.first - left;
            for (const auto coefficient : filter.coefficients) {
                const float pixel = column >= 0 && column < format.width
                    ? image[y * format.width + column] / 255.0f : 1.0f;
                value += coefficient * pixel;
                ++column;
            }
            horizontal[(y + top) * kDeepPrintInputSize + x] = value;
        }
    }

    std::vector<float> output(kDeepPrintInputSize * kDeepPrintInputSize);
    for (int y = 0; y < kDeepPrintInputSize; ++y) {
        const auto& filter = weights[y];
        for (int x = 0; x < kDeepPrintInputSize; ++x) {
            float value = 0.0f;
            int row = filter.first;
            for (const auto coefficient : filter.coefficients) {
                value += coefficient * horizontal[row * kDeepPrintInputSize + x];
                ++row;
            }
            output[y * kDeepPrintInputSize + x] = value;
        }
    }
    return output;
}

} // namespace fingerprint
