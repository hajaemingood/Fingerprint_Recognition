#include "feature/deepprint_preprocessing.h"
#include "feature/template_manager.h"
#include "kisa_api.h"

#include <algorithm>
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

} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc != 2) {
        return 2;
    }
    try {
        const std::filesystem::path references = argv[1];
        for (const int type : {1, 2, 3}) {
            const auto format = fingerprint::imageFormat(type);
            const auto stem = std::to_string(type);
            auto pixels = readValues<unsigned char>(references / (stem + ".raw"),
                                                   format.width * format.height);
            const auto input = fingerprint::preprocessDeepPrint(pixels.data(), format);
            const auto expectedInput = readValues<float>(references / (stem + ".input.f32"), input.size());
            const auto expected = readValues<float>(references / (stem + ".embedding.f32"),
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
