#include "deepprint_preprocessing.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace fingerprint {
namespace {

struct Crop {
    int left;
    int top;
    int width;
    int height;
};

Crop foregroundCrop(const unsigned char* image, ImageFormat format)
{
    constexpr int block = 16;
    constexpr int pixels = block * block;
    constexpr int minimumVariance = 12 * 12;
    constexpr int darkThreshold = 180;
    const int rows = format.height / block;
    const int columns = format.width / block;
    std::vector<int> activeRows(rows);
    std::vector<int> activeColumns(columns);
    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns; ++column) {
            std::int64_t sum = 0;
            std::int64_t squares = 0;
            int dark = 0;
            for (int y = 0; y < block; ++y) {
                const auto* line = image + (row * block + y) * format.width + column * block;
                for (int x = 0; x < block; ++x) {
                    const int pixel = line[x];
                    sum += pixel;
                    squares += pixel * pixel;
                    dark += pixel < darkThreshold;
                }
            }
            if (pixels * squares - sum * sum >= pixels * pixels * minimumVariance && dark * 100 >= pixels * 8) {
                ++activeRows[row];
                ++activeColumns[column];
            }
        }
    }

    const auto measured = [](int cells) { return cells >= 2; };
    if (std::count_if(activeRows.begin(), activeRows.end(), measured) < 4
        || std::count_if(activeColumns.begin(), activeColumns.end(), measured) < 4) {
        return {0, 0, format.width, format.height};
    }
    const auto extent = [measured, block](const std::vector<int>& counts, int dimension) {
        const int first = block * static_cast<int>(std::find_if(counts.begin(), counts.end(), measured) - counts.begin());
        const int last = block * static_cast<int>(std::find_if(counts.rbegin(), counts.rend(), measured).base() - counts.begin());
        const int margin = (last - first + 5) / 10;
        return std::array<int, 2>{std::max(0, first - margin), std::min(dimension, last + margin)};
    };
    const auto x = extent(activeColumns, format.width);
    const auto y = extent(activeRows, format.height);
    return {x[0], y[0], x[1] - x[0], y[1] - y[0]};
}

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

std::vector<float> preprocessDeepPrint(const unsigned char* image, ImageFormat format, bool cropForeground)
{
    const Crop crop = cropForeground ? foregroundCrop(image, format) : Crop{0, 0, format.width, format.height};
    const int side = std::max(crop.width, crop.height);
    const int left = (side - crop.width) / 2;
    const int top = (side - crop.height) / 2;
    const auto weights = makeWeights(side);
    std::vector<float> horizontal(side * kDeepPrintInputSize, 1.0f);

    for (int y = 0; y < crop.height; ++y) {
        const auto* line = image + (y + crop.top) * format.width + crop.left;
        for (int x = 0; x < kDeepPrintInputSize; ++x) {
            const auto& filter = weights[x];
            float value = 0.0f;
            int column = filter.first - left;
            for (const auto coefficient : filter.coefficients) {
                const float pixel = column >= 0 && column < crop.width ? line[column] / 255.0f : 1.0f;
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
