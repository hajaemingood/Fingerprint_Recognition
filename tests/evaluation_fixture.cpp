#define FINGERPRINT_DLL_EXPORTS
#include "kisa_api.h"

#include <windows.h>

int __cdecl KISA_Enroll(int, unsigned char* image, unsigned char* enroll, int* size, int* error, char message[256])
{
    enroll[0] = image[0];
    *size = 1;
    *error = 0;
    message[0] = '\0';
    return 1;
}

int __cdecl KISA_Attempt(int type, unsigned char* image, unsigned char* attempt,
                          int* size, int* error, char message[256])
{
    switch (image[0]) {
    case 20: Sleep(15000); break;
    case 30: ExitProcess(73);
    case 40:
        *size = 0;
        return 1;
    }
    return KISA_Enroll(type, image, attempt, size, error, message);
}

int __cdecl KISA_Compare(unsigned char*, unsigned char* attempt, double* score, int* error, char message[256])
{
    if (attempt[0] != 50) {
        *score = 0.750;
    }
    *error = 0;
    message[0] = '\0';
    return 1;
}
