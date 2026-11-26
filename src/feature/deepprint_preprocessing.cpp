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

std::vector<unsigned char> filterBilateral(const unsigned char* image, ImageFormat format, int rangeSigma)
{
    // Spatial sigma is one pixel. Cache the deployed range sigma across calls.
    const auto makeWeights = [](int sigma) {
        std::array<std::array<float, 256>, 3> table{};
        for (int spatial = 0; spatial <= 2; ++spatial) {
            for (int difference = 0; difference < 256; ++difference) {
                const float range = static_cast<float>(difference) / sigma;
                table[spatial][difference] = std::exp(-0.5f * (spatial + range * range));
            }
        }
        return table;
    };
    static const auto deployedWeights = makeWeights(12);
    decltype(makeWeights(12)) candidateWeights;
    const auto& weights = rangeSigma == 12 ? deployedWeights : (candidateWeights = makeWeights(rangeSigma));

    std::vector<unsigned char> filtered(static_cast<std::size_t>(format.width) * format.height);
    for (int y = 0; y < format.height; ++y) {
        for (int x = 0; x < format.width; ++x) {
            const int center = image[y * format.width + x];
            float total = 0.0f;
            float weighted = 0.0f;
            for (int dy = -1; dy <= 1; ++dy) {
                const auto* row = image + std::clamp(y + dy, 0, format.height - 1) * format.width;
                for (int dx = -1; dx <= 1; ++dx) {
                    const int neighbor = row[std::clamp(x + dx, 0, format.width - 1)];
                    const float weight = weights[dx * dx + dy * dy][std::abs(neighbor - center)];
                    total += weight;
                    weighted += neighbor * weight;
                }
            }
            filtered[y * format.width + x] = static_cast<unsigned char>(std::nearbyint(weighted / total));
        }
    }
    return filtered;
}

Crop foregroundCrop(const unsigned char* image, ImageFormat format, int marginPercent)
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
    const auto extent = [measured, block, marginPercent](const std::vector<int>& counts, int dimension) {
        const int first = block * static_cast<int>(std::find_if(counts.begin(), counts.end(), measured) - counts.begin());
        const int last = block * static_cast<int>(std::find_if(counts.rbegin(), counts.rend(), measured).base() - counts.begin());
        const int margin = ((last - first) * marginPercent + 50) / 100;
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

std::array<ResizeWeights, kDeepPrintInputSize> makeWeights(int side, bool bicubic)
{
    std::array<ResizeWeights, kDeepPrintInputSize> weights;
    const float scale = static_cast<float>(side) / kDeepPrintInputSize;
    const float inverseSupport = 1.0f / std::max(1.0f, scale);
    const float support = std::max(1.0f, scale) * (bicubic ? 2 : 1);
    for (int output = 0; output < kDeepPrintInputSize; ++output) {
        // Match the float32 pixel-center and weighting convention of flx.
        const float center = (output + 0.5f) * scale;
        auto& filter = weights[output];
        filter.first = std::max(0, static_cast<int>(std::ceil(center - support - 0.5f)));
        const int last = std::min(side - 1, static_cast<int>(std::floor(center + support - 0.5f)));
        float sum = 0.0f;
        for (int pixel = filter.first; pixel <= last; ++pixel) {
            const float distance = (pixel - center + 0.5f) * inverseSupport;
            const float d = std::abs(distance);
            float value = std::max(0.0f, 1.0f - d);
            if (bicubic) value = d < 1 ? ((1.5f*d - 2.5f)*d)*d + 1
                                      : ((-0.5f*d + 2.5f)*d - 4)*d + 2;
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

std::vector<float> preprocessDeepPrint(
    const unsigned char* image, ImageFormat format, DeepPrintPreprocessing preprocessing)
{
    return preprocessDeepPrintVariant(image, format, preprocessing, 12, 20, false);
}

std::vector<float> preprocessDeepPrintVariant(const unsigned char* image, ImageFormat format,
    DeepPrintPreprocessing preprocessing, int rangeSigma, int marginPercent, bool bicubic)
{
    std::vector<unsigned char> filtered;
    if (preprocessing == DeepPrintPreprocessing::Bilateral) {
        filtered = filterBilateral(image, format, rangeSigma);
        image = filtered.data();
    }
    if (preprocessing == DeepPrintPreprocessing::ForegroundCenter) {
        const auto box = foregroundCrop(image, format, 0);
        const int dx = static_cast<int>(std::floor((format.width - box.width) * 0.5)) - box.left;
        const int dy = static_cast<int>(std::floor((format.height - box.height) * 0.5)) - box.top;
        filtered.assign(static_cast<size_t>(format.width) * format.height, 255);
        for (int y = std::max(0, -dy); y < std::min(format.height, format.height - dy); ++y) {
            const int first = std::max(0, -dx);
            std::copy(image + y * format.width + first,
                      image + y * format.width + std::min(format.width, format.width - dx),
                      filtered.begin() + ((y + dy) * format.width + dx + first));
        }
        image = filtered.data();
    }
    const Crop crop = preprocessing == DeepPrintPreprocessing::ForegroundCrop
        ? foregroundCrop(image, format, marginPercent) : Crop{0, 0, format.width, format.height};
    const int side = std::max(crop.width, crop.height);
    const int left = (side - crop.width) / 2;
    const int top = (side - crop.height) / 2;
    const auto weights = makeWeights(side, bicubic);
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
            output[y * kDeepPrintInputSize + x] = bicubic ? std::clamp(value, 0.f, 1.f) : value;
        }
    }
    return output;
}

} // namespace fingerprint
