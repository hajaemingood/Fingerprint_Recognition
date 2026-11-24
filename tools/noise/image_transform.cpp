#include "image_transform.h"
#include "gaussian_noise.h"

#include <algorithm>
#include <cmath>

namespace fvc {
namespace {

unsigned char pixelOrWhite(const GrayImage& image, int x, int y)
{
    if (x < 0 || y < 0 || x >= static_cast<int>(image.width) || y >= static_cast<int>(image.height)) {
        return 255;
    }
    return image.pixels[static_cast<std::size_t>(y) * image.width + x];
}

} // namespace

GaussianNoise::GaussianNoise(int sigma, std::uint64_t baseSeed)
    : ImageTransform({TransformKind::Gaussian, sigma, baseSeed}) {}

void GaussianNoise::apply(GrayImage& image, std::uint64_t seed) const
{
    addGaussianNoise(image.pixels, condition().parameter, seed);
}

Rotation::Rotation(int degrees) : ImageTransform({TransformKind::Rotation, degrees}) {}

void Rotation::apply(GrayImage& image, std::uint64_t) const
{
    constexpr double pi = 3.141592653589793;
    const double radians = condition().parameter * pi / 180.0;
    const double cosine = std::cos(radians);
    const double sine = std::sin(radians);
    const double centerX = (image.width - 1.0) / 2.0;
    const double centerY = (image.height - 1.0) / 2.0;
    std::vector<unsigned char> rotated(image.pixels.size());

    for (unsigned int y = 0; y < image.height; ++y) {
        const double dy = y - centerY;
        for (unsigned int x = 0; x < image.width; ++x) {
            const double dx = x - centerX;
            const double sourceX = centerX + dx * cosine - dy * sine;
            const double sourceY = centerY + dx * sine + dy * cosine;
            const int left = static_cast<int>(std::floor(sourceX));
            const int top = static_cast<int>(std::floor(sourceY));
            const double fx = sourceX - left;
            const double fy = sourceY - top;
            const double upper = (1.0 - fx) * pixelOrWhite(image, left, top)
                               + fx * pixelOrWhite(image, left + 1, top);
            const double lower = (1.0 - fx) * pixelOrWhite(image, left, top + 1)
                               + fx * pixelOrWhite(image, left + 1, top + 1);
            rotated[static_cast<std::size_t>(y) * image.width + x] =
                static_cast<unsigned char>(std::lround((1.0 - fy) * upper + fy * lower));
        }
    }
    image.pixels.swap(rotated);
}

Brightness::Brightness(int offset) : ImageTransform({TransformKind::Brightness, offset}) {}

void Brightness::apply(GrayImage& image, std::uint64_t) const
{
    const int offset = condition().parameter;
    for (auto& pixel : image.pixels) {
        pixel = static_cast<unsigned char>(std::clamp(pixel + offset, 0, 255));
    }
}

HorizontalFlip::HorizontalFlip() : ImageTransform({TransformKind::Flip, 0}) {}

void HorizontalFlip::apply(GrayImage& image, std::uint64_t) const
{
    for (unsigned int y = 0; y < image.height; ++y) {
        const auto row = image.pixels.begin() + static_cast<std::size_t>(y) * image.width;
        std::reverse(row, row + image.width);
    }
}

} // namespace fvc
