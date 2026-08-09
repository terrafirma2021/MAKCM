#pragma once

#include <stdint.h>

namespace makcm::ota::board {

// These are semantic partition-layout versions, not hashes of CSV comments.
// Any offset/size/subtype change requires a new constant; packages using the
// old schema are rejected.
constexpr uint32_t kLeftPartitionSchemaV1 = 0x4C010001u;
constexpr uint32_t kRightPartitionSchemaV1 = 0x52010001u;
// Manifest text fields reserve one byte for NUL; hardware_revision is 16 B.
constexpr char kHardwareRevisionR1[] = "makcm-s3-r1";
constexpr char kLeftProjectName[] = "makcm_left_idf";
constexpr char kRightProjectName[] = "makcm_right_idf";
constexpr uint16_t kPairProtocolVersion = 1;

} // namespace makcm::ota::board
