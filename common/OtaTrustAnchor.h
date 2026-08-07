#pragma once

#include <stddef.h>
#include <stdint.h>

namespace makcm::ota::trust {

// ``ota_trust_anchor.local.inc`` is generated from an offline P-256 public
// key.  It must define three constexpr byte arrays named
// kSignerKeyId[16], kPublicX[32], and kPublicY[32].  Private keys never belong
// in firmware or this repository.  An absent local file is intentionally a
// valid build configuration that rejects every signed manifest.
#if defined(MAKCM_OTA_TRUST_ANCHOR_CONFIGURED)
#include "ota_trust_anchor.local.inc"
#elif __has_include("ota_trust_anchor.local.inc")
#include "ota_trust_anchor.local.inc"
#else
constexpr uint8_t kSignerKeyId[16] = {};
constexpr uint8_t kPublicX[32] = {};
constexpr uint8_t kPublicY[32] = {};
#endif

inline bool configured() {
    uint8_t id = 0;
    uint8_t point = 0;
    for (size_t i = 0; i < sizeof(kSignerKeyId); ++i) id |= kSignerKeyId[i];
    for (size_t i = 0; i < sizeof(kPublicX); ++i)
        point |= static_cast<uint8_t>(kPublicX[i] | kPublicY[i]);
    return id != 0 && point != 0;
}

} // namespace makcm::ota::trust
