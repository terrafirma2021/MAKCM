#pragma once

#include <stddef.h>
#include <stdint.h>

#define MAKCM_OTA_PAIR_GENERATION 19
#define MAKCM_OTA_PAIR_GENERATION_TEXT "19"
// The signed manifest and the embedded image identity must both carry the
// exact `makcm-g<generation>` app version.  Derive it from the same macro so
// a release bump cannot leave a stale literal behind in either board.
#define MAKCM_OTA_PAIR_APP_VERSION "makcm-g" MAKCM_OTA_PAIR_GENERATION_TEXT

namespace makcm::ota::build {

// Increment exactly once for a signed pair release. Both board builds include
// this header, preventing independently edited build flags from producing an
// accidental generation mismatch.
constexpr uint32_t kPairGeneration = MAKCM_OTA_PAIR_GENERATION;
constexpr char kPairGenerationText[] = MAKCM_OTA_PAIR_GENERATION_TEXT;
constexpr char kPairAppVersion[] = MAKCM_OTA_PAIR_APP_VERSION;

constexpr uint32_t parseGenerationText(const char *text, uint32_t value = 0) {
    // Keep this C++11-constexpr compatible because the right-board Arduino
    // toolchain does not allow loops or local variables in constexpr bodies.
    return *text == '\0'
               ? value
               : parseGenerationText(text + 1,
                                     value * 10u + static_cast<uint32_t>(*text - '0'));
}
static_assert(parseGenerationText(kPairGenerationText) == kPairGeneration,
              "numeric and text OTA generations must match");

} // namespace makcm::ota::build
