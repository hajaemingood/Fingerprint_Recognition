#include "kisa_api.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <vector>

namespace {

struct ImageCase {
    int type;
    int pixels;
};

bool testPipeline(ImageCase input)
{
    std::vector<unsigned char> image(input.pixels, 0);
    std::vector<unsigned char> enrolled(1000000);
    std::vector<unsigned char> probe(1000000);
    int enrollSize = 0;
    int probeSize = 0;
    int error = 0;
    char message[256]{};
    double score = 0.0;

    if (KISA_Enroll(input.type, image.data(), enrolled.data(), &enrollSize, &error, message) != 1
        || enrollSize <= 0 || enrollSize >= static_cast<int>(enrolled.size())
        || KISA_Attempt(input.type, image.data(), probe.data(), &probeSize, &error, message) != 1
        || probeSize != enrollSize
        || KISA_Compare(enrolled.data(), probe.data(), &score, &error, message) != 1
        || score != 1.0) {
        return false;
    }

    std::fill(image.begin(), image.end(), 255);
    if (KISA_Attempt(input.type, image.data(), probe.data(), &probeSize, &error, message) != 1
        || KISA_Compare(enrolled.data(), probe.data(), &score, &error, message) != 1
        || score != 0.0) {
        return false;
    }

    return KISA_Attempt(-1, image.data(), probe.data(), &probeSize, &error, message) == 0
        && probeSize == 0 && error != 0 && message[0] != '\0'
        && KISA_Compare(nullptr, probe.data(), &score, &error, message) == 0;
}

} // namespace

int main()
{
    constexpr std::array<ImageCase, 7> cases{{
        {1, 448 * 478}, {2, 300 * 300}, {3, 640 * 480}, {5, 280 * 320},
        {7, 292 * 525}, {8, 248 * 292}, {9, 288 * 320},
    }};
    for (const auto input : cases) {
        if (!testPipeline(input)) {
            std::cerr << "Pipeline failed for iType " << input.type << '\n';
            return 1;
        }
    }
    std::cout << "Fingerprint pipeline passed\n";
}
