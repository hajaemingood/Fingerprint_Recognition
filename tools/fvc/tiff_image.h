#pragma once

#include <filesystem>
#include <vector>

namespace fvc {

struct GrayImage {
    int type;
    unsigned int width;
    unsigned int height;
    std::vector<unsigned char> pixels;
};

GrayImage readTiff(const std::filesystem::path& path);
void writeTiff(const std::filesystem::path& path, const GrayImage& image);

} // namespace fvc
