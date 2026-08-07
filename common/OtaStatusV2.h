#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "OtaSnapshotV2.h"

namespace makcm::ota {

constexpr uint8_t kStatusV2Version = 2;
constexpr size_t kStatusV2PayloadSize = 400;

enum StatusV2EvidenceFlags : uint16_t {
    kEvidenceActivePresent = 0x0001,
    kEvidenceCompletedPresent = 0x0002,
    kEvidenceSnapshotValid = 0x0004,
    kEvidenceSnapshotCopyError = 0x0008,
    kEvidenceLocalValidated = 0x0010,
    kEvidencePairCommitted = 0x0020,
    kEvidenceRunningImageHashValid = 0x0040,
    kEvidenceIdentityDigestValid = 0x0080,
};
constexpr uint16_t kKnownStatusV2EvidenceFlags =
    kEvidenceActivePresent | kEvidenceCompletedPresent |
    kEvidenceSnapshotValid | kEvidenceSnapshotCopyError |
    kEvidenceLocalValidated | kEvidencePairCommitted |
    kEvidenceRunningImageHashValid | kEvidenceIdentityDigestValid;
constexpr uint32_t kKnownStatusFlags =
    kStatusPrepared | kStatusPendingVerify | kStatusRollbackSupported |
    kStatusTrustConfigured | kStatusPeerCompatible |
    kStatusMaintenanceExclusive | kStatusHealthPassed |
    kStatusLocalBootValidated;

struct StatusPayloadV2 {
    Target role = Target::Host;
    snapshot::Phase local_phase = snapshot::Phase::Empty;
    State state = State::Idle;
    Command last_command = Command::Hello;
    Command last_response_command = Command::StatusResponse;
    uint8_t last_ota_operation = 0;
    snapshot::Outcome completed_outcome = snapshot::Outcome::None;
    NackReason reason = NackReason::None;
    uint16_t protocol_epoch = snapshot::kProtocolEpoch;
    uint32_t status_flags = 0;
    uint32_t next_offset = 0;
    uint32_t record_transaction_id = 0;
    uint32_t completed_transaction_id = 0;
    uint32_t target_generation = 0;
    uint32_t active_generation = 0;
    uint32_t previous_generation = 0;
    uint32_t partition_schema = 0;
    uint32_t image_size = 0;
    uint32_t record_sequence = 0;
    uint32_t last_request_sequence = 0;
    uint32_t completed_terminal_sequence = 0;
    uint32_t reset_reason = 0;
    int32_t last_ota_error = 0;
    int32_t last_state_query_error = 0;
    uint16_t boot_attempt = 0;
    uint16_t evidence_flags = 0;
    uint8_t running_slot = 0xFF;
    uint8_t boot_slot = 0xFF;
    uint8_t target_slot = 0xFF;
    uint8_t previous_slot = 0xFF;
    uint8_t image_state = 0xFF;
    Command completed_terminal_command = Command::Hello;
    uint32_t record_crc32 = 0;
    uint8_t image_sha256[32]{};
    uint8_t completed_image_sha256[32]{};
    uint8_t running_image_sha256[32]{};
    uint8_t manifest_sha256[32]{};
    uint8_t identity_digest[32]{};
    uint8_t previous_image_sha256[32]{};
    uint8_t app_elf_sha256[32]{};
    char hardware_revision[16]{};
    char project_name[32]{};
    char app_version[32]{};
};

inline bool validNackReason(NackReason reason) {
    return static_cast<uint16_t>(reason) <=
           static_cast<uint16_t>(NackReason::RollbackUnavailable);
}

inline bool validResponseCommand(Command command) {
    return command == Command::Ack || command == Command::Nack ||
           command == Command::StatusResponse;
}

inline bool validStatusV2(const StatusPayloadV2 &status) {
    if ((status.role != Target::Left && status.role != Target::Right) ||
        !snapshot::validPhase(status.local_phase) ||
        static_cast<uint8_t>(status.state) >
            static_cast<uint8_t>(State::Fault) ||
        !validCommand(status.last_command) ||
        !validResponseCommand(status.last_response_command) ||
        !snapshot::validOutcome(status.completed_outcome) ||
        !validNackReason(status.reason) ||
        status.protocol_epoch != snapshot::kProtocolEpoch ||
        // A board that can answer is running an image, so its generation is
        // never zero.  Without this the right board shipped a Status V2 that
        // simply omitted the field and still validated on both sides.
        status.active_generation == 0 ||
        status.record_sequence == 0 ||
        status.record_sequence == UINT32_MAX ||
        (status.status_flags & ~kKnownStatusFlags) != 0 ||
        (status.evidence_flags & ~kKnownStatusV2EvidenceFlags) != 0 ||
        (status.evidence_flags & kEvidenceSnapshotValid) == 0 ||
        !snapshot::validFixedString(status.hardware_revision,
                                    sizeof(status.hardware_revision)) ||
        !snapshot::validFixedString(status.project_name,
                                    sizeof(status.project_name)) ||
        !snapshot::validFixedString(status.app_version,
                                    sizeof(status.app_version)) ||
        snapshot::allZero(status.app_elf_sha256,
                          sizeof(status.app_elf_sha256)))
        return false;
    const bool active =
        (status.evidence_flags & kEvidenceActivePresent) != 0;
    const bool completed =
        (status.evidence_flags & kEvidenceCompletedPresent) != 0;
    if (active &&
        (status.local_phase == snapshot::Phase::Empty ||
         status.local_phase == snapshot::Phase::PairCommitted ||
         status.record_transaction_id == 0 ||
         status.target_generation == 0 ||
         status.previous_generation == 0 ||
         status.partition_schema == 0 || status.image_size == 0 ||
         status.last_request_sequence == 0 ||
         status.last_request_sequence == UINT32_MAX ||
         status.target_slot == 0xFF || status.previous_slot == 0xFF ||
         status.target_slot == status.previous_slot ||
         snapshot::allZero(status.image_sha256, 32) ||
         snapshot::allZero(status.manifest_sha256, 32) ||
         snapshot::allZero(status.identity_digest, 32) ||
         snapshot::allZero(status.previous_image_sha256, 32)))
        return false;
    if (completed &&
        (status.completed_transaction_id == 0 ||
         status.completed_terminal_sequence == 0 ||
         status.completed_terminal_sequence == UINT32_MAX ||
         status.completed_outcome == snapshot::Outcome::None ||
         (status.completed_terminal_command != Command::Confirm &&
          status.completed_terminal_command != Command::Abort) ||
         ((status.completed_outcome == snapshot::Outcome::Confirmed) !=
          (status.completed_terminal_command == Command::Confirm)) ||
         snapshot::allZero(status.completed_image_sha256, 32)))
        return false;
    if (!active && (status.record_transaction_id != 0 ||
                    status.last_request_sequence != 0 ||
                    status.local_phase != snapshot::Phase::Empty))
        return false;
    if (!completed &&
        (status.completed_transaction_id != 0 ||
         status.completed_terminal_sequence != 0 ||
         status.completed_outcome != snapshot::Outcome::None ||
         status.completed_terminal_command != Command::Hello ||
         !snapshot::allZero(status.completed_image_sha256, 32)))
        return false;
    if ((status.evidence_flags & kEvidenceRunningImageHashValid) != 0 &&
        snapshot::allZero(status.running_image_sha256, 32))
        return false;
    if ((status.evidence_flags & kEvidenceIdentityDigestValid) != 0 &&
        snapshot::allZero(status.identity_digest, 32))
        return false;
    return true;
}

inline bool encodeStatusV2(const StatusPayloadV2 &status, uint8_t *wire,
                           size_t capacity) {
    if (wire == nullptr || capacity < kStatusV2PayloadSize ||
        !validStatusV2(status))
        return false;
    memset(wire, 0, kStatusV2PayloadSize);
    wire[0] = kStatusV2Version;
    wire[1] = static_cast<uint8_t>(status.role);
    wire[2] = static_cast<uint8_t>(status.local_phase);
    wire[3] = static_cast<uint8_t>(status.state);
    wire[4] = static_cast<uint8_t>(status.last_command);
    wire[5] = static_cast<uint8_t>(status.last_response_command);
    wire[6] = status.last_ota_operation;
    wire[7] = static_cast<uint8_t>(status.completed_outcome);
    writeLe16(wire + 8, static_cast<uint16_t>(status.reason));
    writeLe16(wire + 10, status.protocol_epoch);
    writeLe32(wire + 12, status.status_flags);
    writeLe32(wire + 16, status.next_offset);
    writeLe32(wire + 20, status.record_transaction_id);
    writeLe32(wire + 24, status.completed_transaction_id);
    writeLe32(wire + 28, status.target_generation);
    writeLe32(wire + 32, status.active_generation);
    writeLe32(wire + 36, status.previous_generation);
    writeLe32(wire + 40, status.partition_schema);
    writeLe32(wire + 44, status.image_size);
    writeLe32(wire + 48, status.record_sequence);
    writeLe32(wire + 52, status.last_request_sequence);
    writeLe32(wire + 56, status.completed_terminal_sequence);
    writeLe32(wire + 60, status.reset_reason);
    writeLe32(wire + 64, static_cast<uint32_t>(status.last_ota_error));
    writeLe32(wire + 68,
              static_cast<uint32_t>(status.last_state_query_error));
    writeLe16(wire + 72, status.boot_attempt);
    writeLe16(wire + 74, status.evidence_flags);
    wire[76] = status.running_slot;
    wire[77] = status.boot_slot;
    wire[78] = status.target_slot;
    wire[79] = status.previous_slot;
    wire[80] = status.image_state;
    wire[81] = static_cast<uint8_t>(status.completed_terminal_command);
    writeLe32(wire + 84, status.record_crc32);
    memcpy(wire + 96, status.image_sha256, 32);
    memcpy(wire + 128, status.completed_image_sha256, 32);
    memcpy(wire + 160, status.running_image_sha256, 32);
    memcpy(wire + 192, status.manifest_sha256, 32);
    memcpy(wire + 224, status.identity_digest, 32);
    memcpy(wire + 256, status.previous_image_sha256, 32);
    memcpy(wire + 288, status.app_elf_sha256, 32);
    memcpy(wire + 320, status.hardware_revision, 16);
    memcpy(wire + 336, status.project_name, 32);
    memcpy(wire + 368, status.app_version, 32);
    return true;
}

inline bool decodeStatusV2(const uint8_t *wire, size_t length,
                           StatusPayloadV2 &status) {
    if (wire == nullptr || length != kStatusV2PayloadSize ||
        wire[0] != kStatusV2Version || wire[82] != 0 || wire[83] != 0)
        return false;
    for (size_t i = 88; i < 96; ++i)
        if (wire[i] != 0) return false;
    memset(&status, 0, sizeof(status));
    status.role = static_cast<Target>(wire[1]);
    status.local_phase = static_cast<snapshot::Phase>(wire[2]);
    status.state = static_cast<State>(wire[3]);
    status.last_command = static_cast<Command>(wire[4]);
    status.last_response_command = static_cast<Command>(wire[5]);
    status.last_ota_operation = wire[6];
    status.completed_outcome = static_cast<snapshot::Outcome>(wire[7]);
    status.reason = static_cast<NackReason>(readLe16(wire + 8));
    status.protocol_epoch = readLe16(wire + 10);
    status.status_flags = readLe32(wire + 12);
    status.next_offset = readLe32(wire + 16);
    status.record_transaction_id = readLe32(wire + 20);
    status.completed_transaction_id = readLe32(wire + 24);
    status.target_generation = readLe32(wire + 28);
    status.active_generation = readLe32(wire + 32);
    status.previous_generation = readLe32(wire + 36);
    status.partition_schema = readLe32(wire + 40);
    status.image_size = readLe32(wire + 44);
    status.record_sequence = readLe32(wire + 48);
    status.last_request_sequence = readLe32(wire + 52);
    status.completed_terminal_sequence = readLe32(wire + 56);
    status.reset_reason = readLe32(wire + 60);
    status.last_ota_error = static_cast<int32_t>(readLe32(wire + 64));
    status.last_state_query_error =
        static_cast<int32_t>(readLe32(wire + 68));
    status.boot_attempt = readLe16(wire + 72);
    status.evidence_flags = readLe16(wire + 74);
    status.running_slot = wire[76];
    status.boot_slot = wire[77];
    status.target_slot = wire[78];
    status.previous_slot = wire[79];
    status.image_state = wire[80];
    status.completed_terminal_command = static_cast<Command>(wire[81]);
    status.record_crc32 = readLe32(wire + 84);
    memcpy(status.image_sha256, wire + 96, 32);
    memcpy(status.completed_image_sha256, wire + 128, 32);
    memcpy(status.running_image_sha256, wire + 160, 32);
    memcpy(status.manifest_sha256, wire + 192, 32);
    memcpy(status.identity_digest, wire + 224, 32);
    memcpy(status.previous_image_sha256, wire + 256, 32);
    memcpy(status.app_elf_sha256, wire + 288, 32);
    memcpy(status.hardware_revision, wire + 320, 16);
    memcpy(status.project_name, wire + 336, 32);
    memcpy(status.app_version, wire + 368, 32);
    return validStatusV2(status);
}

} // namespace makcm::ota
