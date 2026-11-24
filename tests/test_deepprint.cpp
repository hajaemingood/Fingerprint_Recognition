#include "feature/deepprint_preprocessing.h"
#include "feature/template_manager.h"
#include "kisa_api.h"
#include "matching/fingerprint_matcher.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {

template <class T>
std::vector<T> readValues(const std::filesystem::path& path, std::size_t count)
{
    std::ifstream file(path, std::ios::binary);
    std::vector<T> values(count);
    const auto bytes = static_cast<std::streamsize>(count * sizeof(T));
    if (!file.read(reinterpret_cast<char*>(values.data()), bytes) || file.peek() != EOF) {
        throw std::runtime_error("Invalid reference data: " + path.u8string());
    }
    return values;
}

double maximumError(const float* current, const std::vector<float>& expected)
{
    double error = 0.0;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        error = std::max(error, std::abs(double(current[i]) - expected[i]));
    }
    return error;
}

void verifyWeightedComparison()
{
    fingerprint::Features registered{};
    fingerprint::Features probe{};
    registered[0] = 1.0f;
    registered[256] = 1.0f;
    probe[0] = 1.0f;
    probe[257] = 1.0f;
    std::array<unsigned char, fingerprint::kTemplateSize> enrolled{};
    std::array<unsigned char, fingerprint::kTemplateSize> attempted{};
    fingerprint::encodeTemplate(registered, enrolled.data());
    fingerprint::encodeTemplate(probe, attempted.data());

    const double expected = std::round((0.5 + 0.5 * fingerprint::kTextureWeight) * 1000.0) / 1000.0;
    int error = 0;
    char message[256]{};
    double actual = 0.0;
    if (KISA_Compare(enrolled.data(), attempted.data(), &actual, &error, message) != 1
        || std::abs(actual - expected) > 1e-12) {
        throw std::runtime_error("KISA_Compare did not apply the configured branch weights");
    }
}

void verifyEmptyForegroundFallback()
{
    const auto format = fingerprint::imageFormat(3);
    const std::vector<unsigned char> image(format.width * format.height, 255);
    const auto full = fingerprint::preprocessDeepPrint(image.data(), format);
    if (fingerprint::preprocessDeepPrint(image.data(), format, true) != full) {
        throw std::runtime_error("Empty foreground must use the full frame");
    }
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc != 2) {
        return 2;
    }
    try {
        verifyWeightedComparison();
        verifyEmptyForegroundFallback();
        const std::filesystem::path references = argv[1];
        for (const int type : {1, 2, 3}) {
            const auto format = fingerprint::imageFormat(type);
            const auto stem = std::to_string(type);
            auto pixels = readValues<unsigned char>(references / (stem + ".raw"),
                                                   format.width * format.height);
            const bool crop = FINGERPRINT_FVC2004_ROI && type == 3;
            const auto input = fingerprint::preprocessDeepPrint(pixels.data(), format, crop);
            const auto referenceStem = crop ? stem + ".roi" : stem;
            const auto expectedInput = readValues<float>(references / (referenceStem + ".input.f32"), input.size());
            const auto expected = readValues<float>(references / (referenceStem + ".embedding.f32"),
                                                    fingerprint::kFeatureCount);

            std::vector<unsigned char> enrolled(1000000);
            std::vector<unsigned char> probe(1000000);
            int enrollSize = 0;
            int attemptSize = 0;
            int errorCode = 0;
            char message[256]{};
            double score = 0;
            fingerprint::Features embedding{};
            if (KISA_Enroll(type, pixels.data(), enrolled.data(), &enrollSize, &errorCode, message) != 1
                || KISA_Attempt(type, pixels.data(), probe.data(), &attemptSize, &errorCode, message) != 1
                || enrollSize != 2056 || attemptSize != enrollSize
                || !fingerprint::decodeTemplate(enrolled.data(), embedding)
                || KISA_Compare(enrolled.data(), probe.data(), &score, &errorCode, message) != 1
                || score != 1.0) {
                throw std::runtime_error("DeepPrint KISA round trip failed: " + std::string(message));
            }

            const double resizeError = maximumError(input.data(), expectedInput);
            const double modelError = maximumError(embedding.data(), expected);
            std::cout << "iType " << type << ": resize error " << resizeError
                      << ", embedding error " << modelError << '\n';
            if (resizeError > 2e-6 || modelError > 7e-5) {
                throw std::runtime_error("DeepPrint reference parity failed");
            }
            probe[4] = 0;
            if (KISA_Compare(enrolled.data(), probe.data(), &score, &errorCode, message) != 0
                || errorCode != 4) {
                throw std::runtime_error("Invalid template version was accepted");
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
