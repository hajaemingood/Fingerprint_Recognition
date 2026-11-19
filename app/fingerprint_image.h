#pragma once

#include <string>
#include <vector>

struct FingerprintImage {
    int type = 0;
    std::vector<unsigned char> pixels;
};

// The caller initializes COM on the current thread.
bool loadFingerprintImage(const wchar_t* path, FingerprintImage& image, std::wstring& error);
