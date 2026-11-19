#pragma once

#ifdef FINGERPRINT_DLL_EXPORTS
#define FINGERPRINT_API extern "C" __declspec(dllexport)
#else
#define FINGERPRINT_API extern "C" __declspec(dllimport)
#endif

// FVC iType 1: 448x478, 2: 300x300, 3: 640x480.
// KISA iType 5: 280x320, 7: 292x525, 8: 248x292, 9: 288x320.
// image points to tightly packed, top-down 8-bit grayscale pixels.
// Template and error buffers are owned by the caller. Use a 1,000,000-byte
// template buffer and a 256-byte error buffer, as in the KISA sample.
FINGERPRINT_API int __cdecl KISA_Enroll(
    int iType, unsigned char* image, unsigned char* enroll, int* enrollSize,
    int* errorCode, char errorMessage[256]);

FINGERPRINT_API int __cdecl KISA_Attempt(
    int iType, unsigned char* image, unsigned char* attempt, int* attemptSize,
    int* errorCode, char errorMessage[256]);

// Pass templates produced by Enroll and Attempt. Returns 1 on success and
// 0 on error. The score ranges from 0 to 1 with up to three decimal places.
FINGERPRINT_API int __cdecl KISA_Compare(
    unsigned char* enroll, unsigned char* attempt, double* score,
    int* errorCode, char errorMessage[256]);
