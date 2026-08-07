#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "OtaProtocol.h"

namespace makcm::ota {

constexpr uint16_t kManifestVersion = 1;
constexpr uint8_t kImageKindApplication = 1;
constexpr uint8_t kSignatureEcdsaP256Sha256 = 1;
constexpr uint16_t kEsp32S3ChipId = 9;
constexpr size_t kManifestSignedSize = 128;
constexpr size_t kManifestSignatureSize = 64;
constexpr size_t kManifestWireSize = 192;

struct Manifest {
    uint16_t version = 0;
    Target role = Target::Host;
    uint8_t image_kind = 0;
    uint16_t protocol_version = 0;
    uint16_t chip_id = 0;
    uint32_t generation = 0;
    uint32_t min_peer_generation = 0;
    uint32_t max_peer_generation = 0;
    uint32_t partition_schema = 0;
    uint32_t image_size = 0;
    uint16_t chunk_size = 0;
    uint8_t signature_algorithm = 0;
    char hardware_revision[16]{};
    char project_name[32]{};
    uint8_t image_sha256[32]{};
    uint8_t signer_key_id[16]{};
    uint8_t signature[kManifestSignatureSize]{};
};

inline bool boundedString(const char *value, size_t capacity) {
    return value != nullptr && memchr(value, '\0', capacity) != nullptr &&
           value[0] != '\0';
}

inline bool decodeManifest(const uint8_t *wire, size_t length, Manifest &out) {
    if (wire == nullptr || length != kManifestWireSize) return false;
    out = {};
    out.version = readLe16(wire);
    out.role = static_cast<Target>(wire[2]);
    out.image_kind = wire[3];
    out.protocol_version = readLe16(wire + 4);
    out.chip_id = readLe16(wire + 6);
    out.generation = readLe32(wire + 8);
    out.min_peer_generation = readLe32(wire + 12);
    out.max_peer_generation = readLe32(wire + 16);
    out.partition_schema = readLe32(wire + 20);
    out.image_size = readLe32(wire + 24);
    out.chunk_size = readLe16(wire + 28);
    out.signature_algorithm = wire[30];
    if (wire[31] != 0) return false;
    memcpy(out.hardware_revision, wire + 32, sizeof(out.hardware_revision));
    memcpy(out.project_name, wire + 48, sizeof(out.project_name));
    memcpy(out.image_sha256, wire + 80, sizeof(out.image_sha256));
    memcpy(out.signer_key_id, wire + 112, sizeof(out.signer_key_id));
    memcpy(out.signature, wire + kManifestSignedSize, sizeof(out.signature));
    return out.version == kManifestVersion && validTarget(out.role) &&
           out.image_kind == kImageKindApplication &&
           out.chip_id == kEsp32S3ChipId && out.generation != 0 &&
           out.min_peer_generation <= out.max_peer_generation &&
           out.partition_schema != 0 && out.image_size != 0 &&
           out.chunk_size >= kMinChunkSize && out.chunk_size <= kMaxChunkSize &&
           out.signature_algorithm == kSignatureEcdsaP256Sha256 &&
           boundedString(out.hardware_revision, sizeof(out.hardware_revision)) &&
           boundedString(out.project_name, sizeof(out.project_name));
}

inline bool manifestCompatibleWithPeer(const Manifest &manifest,
                                       uint32_t peer_generation) {
    return peer_generation >= manifest.min_peer_generation &&
           peer_generation <= manifest.max_peer_generation;
}

} // namespace makcm::ota
