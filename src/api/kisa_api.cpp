#define FINGERPRINT_DLL_EXPORTS
#include "kisa_api.h"

#include "feature/feature_extractor.h"
#include "feature/template_manager.h"
#include "matching/fingerprint_matcher.h"

#include <cmath>
#include <cstdio>

namespace {

int report(int code, const char* message, int* errorCode, char* errorMessage)
{
    if (errorCode) {
        *errorCode = code;
    }
    if (errorMessage) {
        std::snprintf(errorMessage, 256, "%s", message);
    }
    return code == 0 ? 1 : 0;
}

int extract(int iType, const unsigned char* image, unsigned char* output,
            int* outputSize, int* errorCode, char* errorMessage)
{
    if (outputSize) {
        *outputSize = 0;
    }
    if (!image || !output || !outputSize) {
        return report(1, "Missing image or template output", errorCode, errorMessage);
    }

    fingerprint::Features descriptor{};
    if (!fingerprint::extractFeatures(iType, image, descriptor)) {
        return report(2, "Unsupported iType", errorCode, errorMessage);
    }

    *outputSize = fingerprint::encodeTemplate(descriptor, output);
    return report(0, "", errorCode, errorMessage);
}

} // namespace

int __cdecl KISA_Enroll(int iType, unsigned char* image, unsigned char* enroll,
                       int* enrollSize, int* errorCode, char errorMessage[256])
{
    return extract(iType, image, enroll, enrollSize, errorCode, errorMessage);
}

int __cdecl KISA_Attempt(int iType, unsigned char* image, unsigned char* attempt,
                        int* attemptSize, int* errorCode, char errorMessage[256])
{
    return extract(iType, image, attempt, attemptSize, errorCode, errorMessage);
}

int __cdecl KISA_Compare(unsigned char* enroll, unsigned char* attempt,
                        double* score, int* errorCode, char errorMessage[256])
{
    if (score) {
        *score = 0.0;
    }
    if (!enroll || !attempt || !score) {
        return report(1, "Missing template or score output", errorCode, errorMessage);
    }

    const auto enrolled = fingerprint::decodeTemplate(enroll);
    const auto probe = fingerprint::decodeTemplate(attempt);
    const double similarity = fingerprint::compareFeatures(enrolled, probe);
    *score = std::round(similarity * 1000.0) / 1000.0;
    return report(0, "", errorCode, errorMessage);
}
