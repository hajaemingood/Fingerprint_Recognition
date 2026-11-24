#include "gaussian_noise.h"
#include "image_transform.h"
#include "verification_metrics.h"

#include <cmath>
#include <iostream>

namespace {

bool checkImageTransforms()
{
    const fvc::GrayImage source{1, 3, 3, {0, 10, 20, 30, 40, 50, 60, 245, 255}};
    auto brighter = source;
    fvc::Brightness(15).apply(brighter, 0);
    auto darker = source;
    fvc::Brightness(-15).apply(darker, 0);
    auto flipped = source;
    fvc::HorizontalFlip().apply(flipped, 0);
    auto rotated = source;
    const fvc::Rotation rotation(90);
    const fvc::ImageTransform& transform = rotation;
    transform.apply(rotated, 0);

    return brighter.pixels == std::vector<unsigned char>{15, 25, 35, 45, 55, 65, 75, 255, 255}
        && darker.pixels == std::vector<unsigned char>{0, 0, 5, 15, 25, 35, 45, 230, 240}
        && flipped.pixels == std::vector<unsigned char>{20, 10, 0, 50, 40, 30, 255, 245, 60}
        && rotated.pixels == std::vector<unsigned char>{20, 50, 255, 10, 40, 245, 0, 30, 60}
        && rotated.width == source.width && rotated.height == source.height
        && rotated.type == source.type;
}

bool checkOperatingPoints()
{
    const std::vector<VerificationScore> scores{{0.9, true}, {0.75, false}, {0.3, true}, {0.2, false}};
    const VerificationCounts totals{3, 3};
    const auto exact = equalErrorOperatingPoint(scores, totals);
    const auto rates = ratesAtThreshold(scores, totals, 0.75);
    const auto estimated = equalErrorOperatingPoint({{0.8, true}, {0.7, false}, {0.4, true}}, {3, 1});
    return exact && exact->threshold == 0.3 && exact->averageError() == 1.0 / 3.0
        && rates && rates->fmr == 1.0 / 3.0 && rates->fnmr == 2.0 / 3.0
        && estimated && estimated->threshold == 0.8 && estimated->averageError() == 1.0 / 3.0
        && !equalErrorOperatingPoint({{0.9, true}}, {2, 1})
        && !ratesAtThreshold(scores, {3, 0}, 0.75);
}

} // namespace

int main()
{
    const std::vector<unsigned char> flat(10000, 128);
    auto noisy = flat;
    const auto seed = fvc::noiseSeed(42, "FVC/101_1.tif", 5);
    fvc::addGaussianNoise(noisy, 0.0, seed);
    if (noisy != flat || seed == fvc::noiseSeed(42, "FVC/101_2.tif", 5)
        || seed == fvc::noiseSeed(42, "FVC/101_1.tif", 10)) {
        std::cerr << "Baseline or seed derivation failed\n";
        return 1;
    }
    auto repeat = flat;
    fvc::addGaussianNoise(noisy, 5.0, seed);
    fvc::addGaussianNoise(repeat, 5.0, seed);
    if (noisy == flat || noisy != repeat) {
        std::cerr << "Noise reproducibility failed\n";
        return 1;
    }
    double sum = 0.0;
    double squaredSum = 0.0;
    for (const auto pixel : noisy) {
        const int difference = pixel - 128;
        sum += difference;
        squaredSum += difference * difference;
    }
    const double mean = sum / noisy.size();
    const double stddev = std::sqrt(squaredSum / noisy.size() - mean * mean);
    if (std::abs(mean) > 0.3 || std::abs(stddev - 5.0) > 0.3) {
        std::cerr << "Gaussian noise distribution differs from requested sigma\n";
        return 1;
    }

    std::vector<unsigned char> darkest(1000, 0);
    std::vector<unsigned char> brightest(1000, 255);
    fvc::addGaussianNoise(darkest, 5.0, seed);
    fvc::addGaussianNoise(brightest, 5.0, seed);
    for (std::size_t i = 0; i < darkest.size(); ++i) {
        if (darkest[i] > 50 || brightest[i] < 205
            || (darkest[i] != 0 && brightest[i] != 255)) {
            std::cerr << "Noise clipped incorrectly at a pixel boundary\n";
            return 1;
        }
    }

    if (equalErrorRate({{0.9, true}, {0.1, false}}) != 0.0
        || equalErrorRate({{0.5, true}, {0.5, false}}) != 0.5
        || equalErrorRate({{0.9, false}, {0.1, true}}) != 1.0
        || equalErrorRate({{0.9, true}}).has_value()) {
        std::cerr << "EER threshold handling failed\n";
        return 1;
    }
    if (!checkImageTransforms() || !checkOperatingPoints()) {
        std::cerr << "Image transformation or robustness metric check failed\n";
        return 1;
    }
    std::cout << "Image transformation and metric checks passed\n";
}
