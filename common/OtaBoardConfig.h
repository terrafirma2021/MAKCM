#pragma once

#include <stdint.h>

namespace makcm::ota::board {

// These are semantic partition-layout versions, not hashes of CSV comments.
// Any offset/size/subtype change requires a new constant and a wired full
// migration before packages using that schema are accepted.
constexpr uint32_t kLeftPartitionSchemaV1 = 0x4C010001u;
constexpr uint32_t kRightPartitionSchemaV1 = 0x52010001u;
// Manifest text fields reserve one byte for NUL; hardware_revision is 16 B.
constexpr char kHardwareRevisionR1[] = "makcm-s3-r1";
constexpr char kLeftProjectName[] = "makcm_left_idf";
constexpr char kRightManifestProjectName[] = "makcm_right_arduino";
// Arduino-ESP32 2.0.16 ships precompiled IDF libraries whose app descriptor
// uses this project name. Role, hardware, schema and signature still bind the
// image to MAKCM; this field independently rejects bootloader/merged images.
constexpr char kRightAppDescriptorProjectName[] = "arduino-lib-builder";
constexpr uint16_t kPairProtocolVersion = 1;

} // namespace makcm::ota::board
