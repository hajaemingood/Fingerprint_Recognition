#include "gaussian_noise.h"
#include "verification_metrics.h"

#include <cmath>
#include <iostream>

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
    std::cout << "Noise and EER checks passed\n";
}
