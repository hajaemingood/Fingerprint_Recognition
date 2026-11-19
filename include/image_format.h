#pragma once

namespace fingerprint {

// Shared input dimensions for the KISA API and applications that call it.
struct ImageFormat {
    int width;
    int height;
};

constexpr ImageFormat imageFormat(int iType)
{
    switch (iType) {
    case 1: return {448, 478}; // FVC2000_DB3_B
    case 2: return {300, 300}; // FVC2002_DB3_B
    case 3: return {640, 480}; // FVC2004_DB1_B
    case 5: return {280, 320}; // DIG00
    case 7: return {292, 525}; // PER00
    case 8: return {248, 292}; // NIT00
    case 9: return {288, 320}; // HUN00
    default: return {0, 0};
    }
}

} // namespace fingerprint
