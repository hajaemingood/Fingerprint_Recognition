#pragma once

#include "fingerprint_features.h"

namespace fingerprint {

// The caller provides a KISA template buffer and passes templates produced
// by the same backend. KISA_Compare has no separate buffer length argument.
int encodeTemplate(const Features& features, unsigned char* output);
bool decodeTemplate(const unsigned char* input, Features& features);

} // namespace fingerprint
