#include "gaussian_noise.h"
#include "fvc_dataset.h"
#include "image_transform.h"
#include "tiff_image.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <memory>

namespace {

namespace fs = std::filesystem;
using Transforms = std::vector<std::unique_ptr<fvc::ImageTransform>>;

Transforms noiseTransforms(int argc, wchar_t** argv, std::uint64_t baseSeed)
{
    std::vector<int> levels;
    for (int i = 4; i < argc; ++i) {
        const int level = fvc::parseInteger<int>(fs::path(argv[i]).u8string());
        if (level <= 0 || level > 255) {
            throw std::runtime_error("Noise levels must be integers from 1 to 255; baseline 0 is automatic");
        }
        levels.push_back(level);
    }
    if (levels.empty()) {
        levels = {2, 5, 10};
    }
    std::sort(levels.begin(), levels.end());
    if (std::adjacent_find(levels.begin(), levels.end()) != levels.end()) {
        throw std::runtime_error("Noise levels must be distinct");
    }

    Transforms transforms;
    for (const int sigma : levels) {
        transforms.push_back(std::make_unique<fvc::GaussianNoise>(sigma, baseSeed));
    }
    return transforms;
}

Transforms robustnessTransforms(bool flip)
{
    Transforms transforms;
    for (const int sigma : {2, 5, 10}) {
        for (const std::uint64_t baseSeed : {42, 43}) {
            transforms.push_back(std::make_unique<fvc::GaussianNoise>(sigma, baseSeed));
        }
    }
    for (const int degrees : {-10, -5, 5, 10}) {
        transforms.push_back(std::make_unique<fvc::Rotation>(degrees));
    }
    for (const int offset : {-15, 15}) {
        transforms.push_back(std::make_unique<fvc::Brightness>(offset));
    }
    if (flip) {
        transforms.push_back(std::make_unique<fvc::HorizontalFlip>());
    }
    return transforms;
}

void writeRow(std::ostream& manifest, const fvc::Sample& sample, const fs::path& probe,
              const fvc::Condition& condition, std::uint64_t seed, bool robustness)
{
    manifest << sample.database << ',' << sample.finger << ',' << sample.impression << ','
             << sample.relativePath.generic_u8string() << ',' << probe.generic_u8string() << ','
             << condition.parameter << ',' << condition.baseSeed << ',' << seed;
    if (robustness) {
        manifest << ',' << fvc::transformName(condition.kind);
    }
    manifest << '\n';
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    const bool robustness = argc >= 4 && std::wstring_view(argv[3]) == L"--robustness";
    const bool flip = robustness && argc == 5 && std::wstring_view(argv[4]) == L"--flip";
    if (argc < 4 || (robustness && argc != 4 && !flip)) {
        std::cerr << "Usage: augment_fvc <fvc-root> <new-output-dir> <seed> [sigma ...]\n";
        std::cerr << "       augment_fvc <fvc-root> <new-output-dir> --robustness [--flip]\n";
        return 2;
    }

    try {
        const fs::path source = argv[1];
        const fs::path output = argv[2];
        const auto baseSeed = robustness ? 0 : fvc::parseInteger<std::uint64_t>(fs::path(argv[3]).u8string());
        const fvc::Condition baseline{fvc::TransformKind::Original, 0, baseSeed};
        const auto transforms = robustness ? robustnessTransforms(flip) : noiseTransforms(argc, argv, baseSeed);

        const auto samples = fvc::scanDataset(source);
        fvc::createOutputDirectory(output);
        const auto partialManifest = output / "manifest.csv.tmp";
        std::ofstream manifest(partialManifest);
        manifest.exceptions(std::ios::failbit | std::ios::badbit);
        manifest << (robustness ? fvc::kRobustnessManifestHeader : fvc::kManifestHeader) << '\n';
        std::size_t generated = 0;
        for (const auto& sample : samples) {
            const auto original = fvc::readTiff(source / sample.relativePath);
            const auto sourceName = sample.relativePath.generic_u8string();
            const auto originalSeed = robustness ? 0 : fvc::noiseSeed(baseSeed, sourceName, 0);
            writeRow(manifest, sample, sample.relativePath, baseline, originalSeed, robustness);
            for (const auto& transform : transforms) {
                const auto& condition = transform->condition();
                const auto relative = fvc::probePath(sample, condition, robustness);
                const auto seed = condition.kind == fvc::TransformKind::Gaussian
                    ? fvc::noiseSeed(condition.baseSeed, sourceName, condition.parameter) : 0;
                auto image = original;
                transform->apply(image, seed);
                const auto destination = output / relative;
                fs::create_directories(destination.parent_path());
                fvc::writeTiff(destination, image);
                const auto restored = fvc::readTiff(destination);
                if (restored.type != image.type || restored.pixels != image.pixels) {
                    throw std::runtime_error("TIFF round-trip mismatch: " + destination.u8string());
                }
                ++generated;
                writeRow(manifest, sample, relative, condition, seed, robustness);
            }
        }
        manifest.close();
        fs::rename(partialManifest, output / "manifest.csv");
        std::cout << "Generated and verified " << generated
                  << (robustness ? " augmented TIFFs from " : " noisy TIFFs from ")
                  << samples.size() << " originals.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "augment_fvc: " << error.what() << '\n';
        return 1;
    }
}
