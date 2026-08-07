#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "OtaProtocol.h"

namespace makcm::ota {

constexpr char kEmbeddedIdentityMagic[8] = {
    'M', 'K', 'I', 'M', 'G', 'V', '1', '\0'};
constexpr char kEmbeddedIdentityTrailer[8] = {
    'I', 'D', 'E', 'N', 'D', 'V', '1', '\0'};
constexpr uint16_t kEmbeddedIdentityVersion = 1;

#pragma pack(push, 1)
struct EmbeddedImageIdentity {
    char magic[8];
    uint16_t version;
    uint16_t size;
    uint8_t role;
    uint8_t reserved0[3];
    uint32_t generation;
    uint32_t partition_schema;
    uint16_t protocol_version;
    uint8_t reserved1[2];
    char hardware_revision[16];
    char project_name[32];
    char app_version[32];
    char trailer[8];
};
#pragma pack(pop)

static_assert(sizeof(EmbeddedImageIdentity) == 116,
              "embedded OTA identity layout changed");

inline bool embeddedIdentityTextValid(const char *value, size_t capacity) {
    if (value == nullptr || capacity < 2 || value[0] == '\0') return false;
    const void *terminator = memchr(value, '\0', capacity);
    if (terminator == nullptr) return false;
    const size_t length =
        static_cast<const char *>(terminator) - value;
    for (size_t i = length + 1; i < capacity; ++i)
        if (value[i] != '\0') return false;
    return true;
}

inline bool validEmbeddedImageIdentity(
    const EmbeddedImageIdentity &identity) {
    return memcmp(identity.magic, kEmbeddedIdentityMagic,
                  sizeof(identity.magic)) == 0 &&
           identity.version == kEmbeddedIdentityVersion &&
           identity.size == sizeof(EmbeddedImageIdentity) &&
           (identity.role == static_cast<uint8_t>(Target::Left) ||
            identity.role == static_cast<uint8_t>(Target::Right)) &&
           identity.reserved0[0] == 0 && identity.reserved0[1] == 0 &&
           identity.reserved0[2] == 0 && identity.reserved1[0] == 0 &&
           identity.reserved1[1] == 0 && identity.generation != 0 &&
           identity.partition_schema != 0 &&
           identity.protocol_version != 0 &&
           embeddedIdentityTextValid(identity.hardware_revision,
                                     sizeof(identity.hardware_revision)) &&
           embeddedIdentityTextValid(identity.project_name,
                                     sizeof(identity.project_name)) &&
           embeddedIdentityTextValid(identity.app_version,
                                     sizeof(identity.app_version)) &&
           memcmp(identity.trailer, kEmbeddedIdentityTrailer,
                  sizeof(identity.trailer)) == 0;
}

} // namespace makcm::ota
