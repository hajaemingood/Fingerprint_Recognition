#include "gaussian_noise.h"
#include "fvc_dataset.h"
#include "tiff_image.h"

#include <algorithm>
#include <fstream>
#include <iostream>

int wmain(int argc, wchar_t** argv)
{
    namespace fs = std::filesystem;
    if (argc < 4) {
        std::cerr << "Usage: augment_fvc <fvc-root> <new-output-dir> <seed> [sigma ...]\n";
        return 2;
    }

    try {
        const fs::path source = argv[1];
        const fs::path output = argv[2];
        const auto baseSeed = fvc::parseInteger<std::uint64_t>(fs::path(argv[3]).u8string());
        std::vector<int> levels{0};
        for (int i = 4; i < argc; ++i) {
            const int level = fvc::parseInteger<int>(fs::path(argv[i]).u8string());
            if (level <= 0 || level > 255) {
                throw std::runtime_error("Noise levels must be integers from 1 to 255; baseline 0 is automatic");
            }
            levels.push_back(level);
        }
        if (levels.size() == 1) {
            levels.insert(levels.end(), {2, 5, 10});
        }
        std::sort(levels.begin(), levels.end());
        if (std::adjacent_find(levels.begin(), levels.end()) != levels.end()) {
            throw std::runtime_error("Noise levels must be distinct");
        }

        const auto samples = fvc::scanDataset(source);
        fvc::createOutputDirectory(output);
        const auto partialManifest = output / "manifest.csv.tmp";
        std::ofstream manifest(partialManifest);
        manifest.exceptions(std::ios::failbit | std::ios::badbit);
        manifest << fvc::kManifestHeader << '\n';
        std::size_t generated = 0;
        for (const auto& sample : samples) {
            const auto original = fvc::readTiff(source / sample.relativePath);
            const auto sourceName = sample.relativePath.generic_u8string();
            for (const int sigma : levels) {
                const auto relative = fvc::probePath(sample, sigma);
                const auto seed = fvc::noiseSeed(baseSeed, sourceName, sigma);
                if (sigma != 0) {
                    auto noisy = original;
                    fvc::addGaussianNoise(noisy.pixels, sigma, seed);
                    const auto destination = output / relative;
                    fs::create_directories(destination.parent_path());
                    fvc::writeTiff(destination, noisy);
                    const auto restored = fvc::readTiff(destination);
                    if (restored.type != noisy.type || restored.pixels != noisy.pixels) {
                        throw std::runtime_error("TIFF round-trip mismatch: " + destination.u8string());
                    }
                    ++generated;
                }
                manifest << sample.database << ',' << sample.finger << ',' << sample.impression
                         << ',' << sourceName << ',' << relative.generic_u8string() << ','
                         << sigma << ',' << baseSeed << ',' << seed << '\n';
            }
        }
        manifest.close();
        fs::rename(partialManifest, output / "manifest.csv");
        std::cout << "Generated and verified " << generated << " noisy TIFFs from "
                  << samples.size() << " originals.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "augment_fvc: " << error.what() << '\n';
        return 1;
    }
}
