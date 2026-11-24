#include "gaussian_noise.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <stdexcept>

namespace fvc {

std::uint64_t noiseSeed(std::uint64_t baseSeed, std::string_view source, int sigma)
{
    std::uint64_t hash = 14695981039346656037ULL;
    const auto mix = [&hash](unsigned char byte) {
        hash = (hash ^ byte) * 1099511628211ULL;
    };
    for (int shift = 0; shift < 64; shift += 8) {
        mix(static_cast<unsigned char>(baseSeed >> shift));
    }
    for (const unsigned char character : source) {
        mix(character);
    }
    mix(0);
    for (int shift = 0; shift < 32; shift += 8) {
        mix(static_cast<unsigned char>(static_cast<std::uint32_t>(sigma) >> shift));
    }
    return hash;
}

void addGaussianNoise(std::vector<unsigned char>& pixels, double sigma, std::uint64_t seed)
{
    if (!std::isfinite(sigma) || sigma < 0.0 || sigma > 255.0) {
        throw std::invalid_argument("Sigma must be between 0 and 255");
    }
    if (sigma == 0.0) {
        return;
    }

    std::mt19937_64 random(seed);
    std::normal_distribution<double> gaussian(0.0, sigma);
    for (auto& pixel : pixels) {
        const double noisy = static_cast<double>(pixel) + gaussian(random);
        pixel = static_cast<unsigned char>(std::lround(std::clamp(noisy, 0.0, 255.0)));
    }
}

} // namespace fvc
