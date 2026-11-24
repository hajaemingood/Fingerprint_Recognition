#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace fvc {

std::uint64_t noiseSeed(std::uint64_t baseSeed, std::string_view source, int sigma);
void addGaussianNoise(std::vector<unsigned char>& pixels, double sigma, std::uint64_t seed);

} // namespace fvc
