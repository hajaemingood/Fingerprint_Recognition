#include "feature_extractor.h"
#include "image_format.h"

namespace fingerprint {

bool extractFeatures(int iType, const unsigned char* image, Features& features)
{
    const auto format = imageFormat(iType);
    const int count = format.width * format.height;
    if (count == 0) {
        return false;
    }

    std::array<int, kFeatureCount> histogram{};
    for (int i = 0; i < count; ++i) {
        ++histogram[image[i] / 16];
    }

    for (std::size_t i = 0; i < features.size(); ++i) {
        features[i] = static_cast<unsigned char>(255 * histogram[i] / count);
    }
    return true;
}

} // namespace fingerprint
