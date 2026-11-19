#pragma once

#include "fingerprint_features.h"

namespace fingerprint {

// The draft format is a fixed-length histogram. The caller provides a
// writable KISA template buffer and passes only generated templates to decode.
int encodeTemplate(const Features& features, unsigned char* output);
Features decodeTemplate(const unsigned char* input);

} // namespace fingerprint
