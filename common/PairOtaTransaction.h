#pragma once

// Pair-coordinator persistence is deliberately independent from either
// board's local OtaSnapshotV2 record.  The left board stores this record in
// two NVS keys and uses it only as a coordinator fact/tombstone.  It never
// treats a request header or a generation-only status as proof of completion.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "OtaStatusV2.h"

namespace makcm::ota {

constexpr uint32_t kPairRecordMagic = 0x32525450u; // "PTR2" little-endian
constexpr uint16_t kPairRecordVersion = 2;
constexpr size_t kPairBoardHeaderSize = 80;
constexpr size_t kPairBoardWireSize = 320;
constexpr size_t kPairTombstoneWireSize = 224;
constexpr size_t kPairRecordHeaderSize = 24;
constexpr size_t kPairRecordCrcOffset =
    kPairRecordHeaderSize + 2 * kPairBoardWireSize +
    kPairTombstoneWireSize;
constexpr size_t kPairRecordWireSize = kPairRecordCrcOffset + 4 + 4;

// Pair lifecycle.  The coordinator intentionally does not implement the
// Continue/Rollback operations itself.  Any non-terminal record is a durable
// hand-off to the Linux host, and the data plane remains fail-closed until
// the host supplies exact facts for the next transition.
enum class PairStage : uint8_t {
    None = 0,
    RightPrepareIntent = 1,
    LeftPrepareIntent = 2,
    RightPrepared = 3,
    BothPrepared = 4,
    RightActivateIntent = 5,
    RightLocalValidated = 6,
    LeftActivateIntent = 7,
    BothLocalValidated = 8,
    PairCommitIntent = 9,
    RightPairCommitted = 10,
    BothPairCommitted = 11,
    RollbackIntent = 12,
    RolledBack = 13,
    MaintenanceRequired = 14,
};

// Kept as a diagnostic result rather than an automatic executor.  Returning
// HostReconcile for unfinished records makes it impossible for a caller to
// accidentally interpret a calculation as an executed multi-board action.
enum class RecoveryAction : uint8_t {
    None = 0,
    HostReconcile = 1,
    EnterSafeMaintenance = 2,
};

struct PairBoardFact {
    bool present = false;
    bool identity_complete = false;
    bool local_validated = false;
    bool pair_committed = false;
    Target role = Target::Host;
    snapshot::Phase local_phase = snapshot::Phase::Empty;
    uint8_t image_state = 0xFF;
    uint8_t running_slot = 0xFF;
    uint8_t boot_slot = 0xFF;
    uint8_t target_slot = 0xFF;
    uint8_t previous_slot = 0xFF;
    Command last_command = Command::Hello;
    Command last_response_command = Command::StatusResponse;
    NackReason last_error = NackReason::None;
    snapshot::Outcome completed_outcome = snapshot::Outcome::None;
    Command completed_terminal_command = static_cast<Command>(0);
    uint16_t protocol_epoch = snapshot::kProtocolEpoch;
    uint32_t transaction_id = 0;
    uint32_t target_generation = 0;
    uint32_t previous_generation = 0;
    uint32_t partition_schema = 0;
    uint32_t image_size = 0;
    uint32_t record_sequence = 0;
    uint32_t last_request_sequence = 0;
    uint32_t completed_transaction_id = 0;
    uint32_t completed_terminal_sequence = 0;
    uint32_t reset_reason = 0;
    int32_t last_ota_error = 0;
    int32_t last_state_query_error = 0;
    uint16_t boot_attempt = 0;
    uint8_t image_sha256[32]{};
    uint8_t manifest_sha256[32]{};
    uint8_t running_image_sha256[32]{};
    uint8_t identity_digest[32]{};
    uint8_t previous_image_sha256[32]{};
    char hardware_revision[16]{};
    char project_name[32]{};
    char app_version[32]{};
};

struct PairCompletedTombstone {
    bool present = false;
    snapshot::Outcome outcome = snapshot::Outcome::None;
    Command terminal_command = Command::Hello;
    uint32_t transaction_id = 0;
    uint32_t target_generation = 0;
    uint32_t terminal_sequence = 0;
    uint32_t record_sequence = 0;
    uint8_t left_target_slot = 0xFF;
    uint8_t right_target_slot = 0xFF;
    uint8_t left_image_sha256[32]{};
    uint8_t right_image_sha256[32]{};
    uint8_t left_manifest_sha256[32]{};
    uint8_t right_manifest_sha256[32]{};
    uint8_t left_identity_digest[32]{};
    uint8_t right_identity_digest[32]{};
};

struct PairTransactionRecord {
    uint32_t magic = kPairRecordMagic;
    uint16_t version = kPairRecordVersion;
    uint16_t size = static_cast<uint16_t>(kPairRecordWireSize);
    uint32_t record_sequence = 0;
    PairStage stage = PairStage::None;
    uint8_t reserved_flags = 0;
    uint16_t reserved = 0;
    uint32_t transaction_id = 0;
    uint32_t target_generation = 0;
    PairBoardFact left{};
    PairBoardFact right{};
    PairCompletedTombstone completed{};
    uint32_t crc = 0;
};

inline bool validPairStage(PairStage stage) {
    return static_cast<uint8_t>(stage) <=
           static_cast<uint8_t>(PairStage::MaintenanceRequired);
}

inline bool canonicalPairString(const char *value, size_t capacity) {
    if (value == nullptr || capacity < 2) return false;
    size_t length = 0;
    while (length < capacity && value[length] != '\0') ++length;
    if (length == 0 || length >= capacity) return false;
    for (size_t i = length + 1; i < capacity; ++i)
        if (value[i] != '\0') return false;
    return true;
}

inline bool validPairBoardFact(const PairBoardFact &fact,
                               Target expected_role) {
    if (!fact.present || fact.role != expected_role ||
        !snapshot::validPhase(fact.local_phase) ||
        !validCommand(fact.last_command) ||
        !validResponseCommand(fact.last_response_command) ||
        !validNackReason(fact.last_error) || fact.transaction_id == 0 ||
        fact.target_generation == 0 || fact.record_sequence == 0 ||
        fact.record_sequence == UINT32_MAX ||
        fact.last_request_sequence == 0 ||
        fact.last_request_sequence == UINT32_MAX)
        return false;
    const bool intent_only =
        fact.local_phase == snapshot::Phase::Receiving ||
        fact.local_phase == snapshot::Phase::ActivateIntent;
    if (fact.identity_complete &&
        (fact.target_slot == 0xFF || fact.running_slot == 0xFF ||
         fact.boot_slot == 0xFF ||
         fact.previous_slot == 0xFF || fact.target_slot == fact.previous_slot ||
         fact.protocol_epoch != snapshot::kProtocolEpoch ||
         fact.previous_generation == 0 || fact.partition_schema == 0 ||
         fact.image_size == 0 ||
         snapshot::allZero(fact.image_sha256, sizeof(fact.image_sha256)) ||
         snapshot::allZero(fact.manifest_sha256,
                           sizeof(fact.manifest_sha256)) ||
         snapshot::allZero(fact.running_image_sha256,
                           sizeof(fact.running_image_sha256)) ||
         snapshot::allZero(fact.identity_digest,
                          sizeof(fact.identity_digest)) ||
         snapshot::allZero(fact.previous_image_sha256,
                           sizeof(fact.previous_image_sha256)) ||
         !canonicalPairString(fact.hardware_revision,
                              sizeof(fact.hardware_revision)) ||
         !canonicalPairString(fact.project_name, sizeof(fact.project_name)) ||
         !canonicalPairString(fact.app_version, sizeof(fact.app_version))))
        return false;
    if (!fact.identity_complete &&
        (!intent_only ||
         snapshot::allZero(fact.image_sha256, sizeof(fact.image_sha256)) ||
         snapshot::allZero(fact.manifest_sha256,
                           sizeof(fact.manifest_sha256))))
        return false;
    if (fact.local_validated &&
        (fact.local_phase != snapshot::Phase::LocalValidated &&
         fact.local_phase != snapshot::Phase::PairPending &&
         fact.local_phase != snapshot::Phase::PairCommitted))
        return false;
    if (fact.pair_committed &&
        (fact.completed_transaction_id != fact.transaction_id ||
         fact.completed_outcome != snapshot::Outcome::Confirmed ||
         fact.completed_terminal_command != Command::Confirm ||
         fact.completed_terminal_sequence == 0 ||
         fact.completed_terminal_sequence == UINT32_MAX))
        return false;
    if (!fact.pair_committed) {
        if (fact.completed_outcome == snapshot::Outcome::None) {
            if (fact.completed_transaction_id != 0 ||
                fact.completed_terminal_sequence != 0 ||
                static_cast<uint8_t>(fact.completed_terminal_command) != 0)
                return false;
        } else if ((fact.completed_outcome == snapshot::Outcome::Aborted ||
                    fact.completed_outcome == snapshot::Outcome::RolledBack) &&
                   fact.local_phase == snapshot::Phase::RolledBack &&
                   fact.completed_transaction_id == fact.transaction_id &&
                   fact.completed_terminal_command == Command::Abort &&
                   fact.completed_terminal_sequence != 0 &&
                   fact.completed_terminal_sequence != UINT32_MAX) {
            // A board-local ABORT tombstone is expected to be non-committed;
            // the pair coordinator promotes it to a pair RolledBack
            // tombstone only after both members prove their previous slots.
        } else {
            return false;
        }
    }
    if (fact.local_phase == snapshot::Phase::Empty &&
        (!fact.pair_committed || !fact.local_validated))
        return false;
    return true;
}

inline bool validPairTombstone(const PairCompletedTombstone &completed) {
    if (!completed.present || completed.transaction_id == 0 ||
        completed.target_generation == 0 || completed.terminal_sequence == 0 ||
        completed.terminal_sequence == UINT32_MAX ||
        completed.record_sequence == 0 ||
        completed.record_sequence == UINT32_MAX ||
        !validCommand(completed.terminal_command) ||
        !snapshot::validOutcome(completed.outcome) ||
        completed.outcome == snapshot::Outcome::None ||
        completed.left_target_slot == 0xFF ||
        completed.right_target_slot == 0xFF)
        return false;
    if (completed.outcome == snapshot::Outcome::Confirmed &&
        completed.terminal_command != Command::Confirm)
        return false;
    if (completed.outcome != snapshot::Outcome::Confirmed &&
        completed.terminal_command != Command::Abort)
        return false;
    return !snapshot::allZero(completed.left_image_sha256, 32) &&
           !snapshot::allZero(completed.right_image_sha256, 32) &&
           !snapshot::allZero(completed.left_manifest_sha256, 32) &&
           !snapshot::allZero(completed.right_manifest_sha256, 32) &&
           !snapshot::allZero(completed.left_identity_digest, 32) &&
           !snapshot::allZero(completed.right_identity_digest, 32);
}

inline uint32_t pairRecordCrc(const PairTransactionRecord &record);

inline bool validPairRecord(const PairTransactionRecord &record) {
    if (record.magic != kPairRecordMagic ||
        record.version != kPairRecordVersion ||
        record.size != kPairRecordWireSize || record.reserved_flags != 0 ||
        record.reserved != 0 || record.record_sequence == 0 ||
        record.record_sequence == UINT32_MAX || !validPairStage(record.stage))
        return false;

    if (record.crc != pairRecordCrc(record)) return false;

    if (record.stage == PairStage::None) {
        return record.transaction_id == 0 && record.target_generation == 0 &&
               !record.left.present && !record.right.present &&
               !record.completed.present;
    }
    if (record.transaction_id == 0 || record.target_generation == 0 ||
        (record.left.present &&
         !validPairBoardFact(record.left, Target::Left)) ||
        (record.right.present &&
         !validPairBoardFact(record.right, Target::Right)))
        return false;
    if ((record.left.present &&
         (record.left.transaction_id != record.transaction_id ||
          record.left.target_generation != record.target_generation)) ||
        (record.right.present &&
         (record.right.transaction_id != record.transaction_id ||
          record.right.target_generation != record.target_generation)))
        return false;

    const bool left_valid = validPairBoardFact(record.left, Target::Left);
    const bool right_valid = validPairBoardFact(record.right, Target::Right);
    const bool same_identity =
        left_valid && right_valid &&
        record.left.transaction_id == record.transaction_id &&
        record.right.transaction_id == record.transaction_id &&
        record.left.target_generation == record.target_generation &&
        record.right.target_generation == record.target_generation;
    switch (record.stage) {
    case PairStage::RightPrepareIntent:
        return right_valid &&
               record.right.local_phase == snapshot::Phase::Receiving;
    case PairStage::LeftPrepareIntent:
        return same_identity &&
               record.left.local_phase == snapshot::Phase::Receiving;
    case PairStage::RightPrepared:
        return right_valid &&
               record.right.identity_complete &&
               record.right.local_phase == snapshot::Phase::Prepared;
    case PairStage::BothPrepared:
        return same_identity &&
               record.left.local_phase == snapshot::Phase::Prepared &&
               record.right.local_phase == snapshot::Phase::Prepared;
    case PairStage::RightActivateIntent:
        return right_valid &&
               record.right.identity_complete &&
               (record.right.local_phase == snapshot::Phase::ActivateIntent ||
                record.right.local_phase == snapshot::Phase::BootSelected ||
                record.right.local_phase == snapshot::Phase::PendingVerify);
    case PairStage::RightLocalValidated:
        return right_valid && record.right.local_validated;
    case PairStage::LeftActivateIntent:
        return same_identity && record.right.local_validated &&
               record.left.identity_complete &&
               (record.left.local_phase == snapshot::Phase::ActivateIntent ||
                record.left.local_phase == snapshot::Phase::BootSelected ||
                record.left.local_phase == snapshot::Phase::PendingVerify);
    case PairStage::BothLocalValidated:
    case PairStage::PairCommitIntent:
        return same_identity && record.left.local_validated &&
               record.right.local_validated;
    case PairStage::RightPairCommitted:
        return same_identity && record.right.pair_committed &&
               record.left.local_validated;
    case PairStage::BothPairCommitted:
        return same_identity && record.left.pair_committed &&
               record.right.pair_committed &&
               validPairTombstone(record.completed) &&
               record.completed.outcome == snapshot::Outcome::Confirmed &&
               record.completed.transaction_id == record.transaction_id &&
               record.completed.target_generation == record.target_generation;
    case PairStage::RolledBack:
        return same_identity && record.left.identity_complete &&
               record.right.identity_complete &&
               record.left.running_slot == record.left.previous_slot &&
               record.left.boot_slot == record.left.previous_slot &&
               record.right.running_slot == record.right.previous_slot &&
               record.right.boot_slot == record.right.previous_slot &&
               memcmp(record.left.running_image_sha256,
                      record.left.previous_image_sha256, 32) == 0 &&
               memcmp(record.right.running_image_sha256,
                      record.right.previous_image_sha256, 32) == 0 &&
               validPairTombstone(record.completed) &&
               record.completed.outcome == snapshot::Outcome::RolledBack;
    case PairStage::RollbackIntent:
        return left_valid || right_valid;
    case PairStage::MaintenanceRequired:
        return left_valid || right_valid;
    case PairStage::None:
        break;
    }
    return false;
}

// Explicit little-endian wire helpers.  The CRC covers every byte before the
// CRC field, including reserved bytes.  This avoids relying on compiler
// struct packing for NVS records.
inline void pairWriteBoard(const PairBoardFact &fact, uint8_t *wire) {
    memset(wire, 0, kPairBoardWireSize);
    wire[0] = fact.present ? 1 : 0;
    if (!fact.present) return;
    wire[1] = fact.identity_complete ? 1 : 0;
    wire[2] = fact.local_validated ? 1 : 0;
    wire[3] = fact.pair_committed ? 1 : 0;
    wire[4] = static_cast<uint8_t>(fact.role);
    wire[5] = static_cast<uint8_t>(fact.local_phase);
    wire[6] = fact.image_state;
    wire[7] = fact.running_slot;
    wire[8] = fact.boot_slot;
    wire[9] = fact.target_slot;
    wire[10] = fact.previous_slot;
    wire[11] = static_cast<uint8_t>(fact.last_command);
    wire[12] = static_cast<uint8_t>(fact.last_response_command);
    wire[13] = static_cast<uint8_t>(fact.completed_outcome);
    wire[14] = static_cast<uint8_t>(fact.completed_terminal_command);
    writeLe16(wire + 16, static_cast<uint16_t>(fact.last_error));
    writeLe16(wire + 18, fact.protocol_epoch);
    writeLe32(wire + 20, fact.transaction_id);
    writeLe32(wire + 24, fact.target_generation);
    writeLe32(wire + 28, fact.previous_generation);
    writeLe32(wire + 32, fact.partition_schema);
    writeLe32(wire + 36, fact.image_size);
    writeLe32(wire + 40, fact.record_sequence);
    writeLe32(wire + 44, fact.last_request_sequence);
    writeLe32(wire + 48, fact.completed_transaction_id);
    writeLe32(wire + 52, fact.completed_terminal_sequence);
    writeLe32(wire + 56, fact.reset_reason);
    writeLe32(wire + 60, static_cast<uint32_t>(fact.last_ota_error));
    writeLe32(wire + 64, static_cast<uint32_t>(fact.last_state_query_error));
    writeLe16(wire + 68, fact.boot_attempt);
    memcpy(wire + 80, fact.image_sha256, 32);
    memcpy(wire + 112, fact.manifest_sha256, 32);
    memcpy(wire + 144, fact.running_image_sha256, 32);
    memcpy(wire + 176, fact.identity_digest, 32);
    memcpy(wire + 208, fact.previous_image_sha256, 32);
    memcpy(wire + 240, fact.hardware_revision,
           sizeof(fact.hardware_revision));
    memcpy(wire + 256, fact.project_name, sizeof(fact.project_name));
    memcpy(wire + 288, fact.app_version, sizeof(fact.app_version));
}

inline void pairReadBoard(const uint8_t *wire, PairBoardFact &fact) {
    memset(&fact, 0, sizeof(fact));
    fact.present = wire[0] == 1;
    fact.identity_complete = wire[1] == 1;
    fact.local_validated = wire[2] == 1;
    fact.pair_committed = wire[3] == 1;
    fact.role = static_cast<Target>(wire[4]);
    fact.local_phase = static_cast<snapshot::Phase>(wire[5]);
    fact.image_state = wire[6];
    fact.running_slot = wire[7];
    fact.boot_slot = wire[8];
    fact.target_slot = wire[9];
    fact.previous_slot = wire[10];
    fact.last_command = static_cast<Command>(wire[11]);
    fact.last_response_command = static_cast<Command>(wire[12]);
    fact.completed_outcome = static_cast<snapshot::Outcome>(wire[13]);
    fact.completed_terminal_command = static_cast<Command>(wire[14]);
    fact.last_error = static_cast<NackReason>(readLe16(wire + 16));
    fact.protocol_epoch = readLe16(wire + 18);
    fact.transaction_id = readLe32(wire + 20);
    fact.target_generation = readLe32(wire + 24);
    fact.previous_generation = readLe32(wire + 28);
    fact.partition_schema = readLe32(wire + 32);
    fact.image_size = readLe32(wire + 36);
    fact.record_sequence = readLe32(wire + 40);
    fact.last_request_sequence = readLe32(wire + 44);
    fact.completed_transaction_id = readLe32(wire + 48);
    fact.completed_terminal_sequence = readLe32(wire + 52);
    fact.reset_reason = readLe32(wire + 56);
    fact.last_ota_error = static_cast<int32_t>(readLe32(wire + 60));
    fact.last_state_query_error = static_cast<int32_t>(readLe32(wire + 64));
    fact.boot_attempt = readLe16(wire + 68);
    memcpy(fact.image_sha256, wire + 80, 32);
    memcpy(fact.manifest_sha256, wire + 112, 32);
    memcpy(fact.running_image_sha256, wire + 144, 32);
    memcpy(fact.identity_digest, wire + 176, 32);
    memcpy(fact.previous_image_sha256, wire + 208, 32);
    memcpy(fact.hardware_revision, wire + 240,
           sizeof(fact.hardware_revision));
    memcpy(fact.project_name, wire + 256, sizeof(fact.project_name));
    memcpy(fact.app_version, wire + 288, sizeof(fact.app_version));
}

inline void pairWriteTombstone(const PairCompletedTombstone &completed,
                               uint8_t *wire) {
    memset(wire, 0, kPairTombstoneWireSize);
    wire[0] = completed.present ? 1 : 0;
    if (!completed.present) return;
    wire[1] = static_cast<uint8_t>(completed.outcome);
    wire[2] = static_cast<uint8_t>(completed.terminal_command);
    writeLe32(wire + 4, completed.transaction_id);
    writeLe32(wire + 8, completed.target_generation);
    writeLe32(wire + 12, completed.terminal_sequence);
    writeLe32(wire + 16, completed.record_sequence);
    wire[20] = completed.left_target_slot;
    wire[21] = completed.right_target_slot;
    memcpy(wire + 24, completed.left_image_sha256, 32);
    memcpy(wire + 56, completed.right_image_sha256, 32);
    memcpy(wire + 88, completed.left_manifest_sha256, 32);
    memcpy(wire + 120, completed.right_manifest_sha256, 32);
    memcpy(wire + 152, completed.left_identity_digest, 32);
    memcpy(wire + 184, completed.right_identity_digest, 32);
}

inline void pairReadTombstone(const uint8_t *wire,
                              PairCompletedTombstone &completed) {
    memset(&completed, 0, sizeof(completed));
    completed.present = wire[0] == 1;
    completed.outcome = static_cast<snapshot::Outcome>(wire[1]);
    completed.terminal_command = static_cast<Command>(wire[2]);
    completed.transaction_id = readLe32(wire + 4);
    completed.target_generation = readLe32(wire + 8);
    completed.terminal_sequence = readLe32(wire + 12);
    completed.record_sequence = readLe32(wire + 16);
    completed.left_target_slot = wire[20];
    completed.right_target_slot = wire[21];
    memcpy(completed.left_image_sha256, wire + 24, 32);
    memcpy(completed.right_image_sha256, wire + 56, 32);
    memcpy(completed.left_manifest_sha256, wire + 88, 32);
    memcpy(completed.right_manifest_sha256, wire + 120, 32);
    memcpy(completed.left_identity_digest, wire + 152, 32);
    memcpy(completed.right_identity_digest, wire + 184, 32);
}

inline bool pairBoardWireCanonical(const uint8_t *wire) {
    if (wire == nullptr || wire[0] > 1 || wire[1] > 1 || wire[2] > 1 ||
        wire[3] > 1 || wire[15] != 0)
        return false;
    for (size_t i = 70; i < kPairBoardHeaderSize; ++i)
        if (wire[i] != 0) return false;
    if (wire[0] == 0)
        for (size_t i = 1; i < kPairBoardWireSize; ++i)
            if (wire[i] != 0) return false;
    return true;
}

inline bool pairTombstoneWireCanonical(const uint8_t *wire) {
    if (wire == nullptr || wire[0] > 1 || wire[3] != 0 || wire[22] != 0 ||
        wire[23] != 0)
        return false;
    for (size_t i = 216; i < kPairTombstoneWireSize; ++i)
        if (wire[i] != 0) return false;
    if (wire[0] == 0)
        for (size_t i = 1; i < kPairTombstoneWireSize; ++i)
            if (wire[i] != 0) return false;
    return true;
}

inline void encodePairRecordFields(const PairTransactionRecord &record,
                                   uint8_t *wire) {
    memset(wire, 0, kPairRecordWireSize);
    writeLe32(wire, kPairRecordMagic);
    writeLe16(wire + 4, kPairRecordVersion);
    writeLe16(wire + 6, static_cast<uint16_t>(kPairRecordWireSize));
    writeLe32(wire + 8, record.record_sequence);
    wire[12] = static_cast<uint8_t>(record.stage);
    wire[13] = record.reserved_flags;
    writeLe16(wire + 14, record.reserved);
    writeLe32(wire + 16, record.transaction_id);
    writeLe32(wire + 20, record.target_generation);
    pairWriteBoard(record.left, wire + kPairRecordHeaderSize);
    pairWriteBoard(record.right,
                   wire + kPairRecordHeaderSize + kPairBoardWireSize);
    pairWriteTombstone(record.completed,
                       wire + kPairRecordHeaderSize + 2 * kPairBoardWireSize);
}

inline uint32_t pairRecordCrc(const PairTransactionRecord &record) {
    // Pair writes are serialized by the left-board OTA mutex.  Keeping this
    // scratch out of the caller stack is important because PairRecordV2 is
    // intentionally larger than the old V1 record.
    static uint8_t scratch[kPairRecordWireSize];
    encodePairRecordFields(record, scratch);
    return crc32(scratch, kPairRecordCrcOffset);
}

inline void sealPairRecord(PairTransactionRecord &record) {
    record.magic = kPairRecordMagic;
    record.version = kPairRecordVersion;
    record.size = static_cast<uint16_t>(kPairRecordWireSize);
    record.reserved_flags = 0;
    record.reserved = 0;
    record.crc = pairRecordCrc(record);
}

inline bool encodePairRecord(const PairTransactionRecord &record,
                             uint8_t *wire, size_t capacity) {
    if (wire == nullptr || capacity < kPairRecordWireSize ||
        !validPairRecord(record))
        return false;
    encodePairRecordFields(record, wire);
    writeLe32(wire + kPairRecordCrcOffset, record.crc);
    // The four bytes after CRC are reserved for future authenticated metadata
    // and must remain zero for deterministic conflict detection.
    memset(wire + kPairRecordCrcOffset + 4, 0, 4);
    return true;
}

inline bool decodePairRecord(const uint8_t *wire, size_t length,
                             PairTransactionRecord &record) {
    if (wire == nullptr || length != kPairRecordWireSize ||
        readLe32(wire) != kPairRecordMagic ||
        readLe16(wire + 4) != kPairRecordVersion ||
        readLe16(wire + 6) != kPairRecordWireSize || wire[13] != 0 ||
        readLe16(wire + 14) != 0 ||
        readLe32(wire + kPairRecordCrcOffset + 4) != 0 ||
        !pairBoardWireCanonical(wire + kPairRecordHeaderSize) ||
        !pairBoardWireCanonical(wire + kPairRecordHeaderSize +
                                kPairBoardWireSize) ||
        !pairTombstoneWireCanonical(
            wire + kPairRecordHeaderSize + 2 * kPairBoardWireSize) ||
        crc32(wire, kPairRecordCrcOffset) !=
            readLe32(wire + kPairRecordCrcOffset))
        return false;
    memset(&record, 0, sizeof(record));
    record.magic = readLe32(wire);
    record.version = readLe16(wire + 4);
    record.size = readLe16(wire + 6);
    record.record_sequence = readLe32(wire + 8);
    record.stage = static_cast<PairStage>(wire[12]);
    record.reserved_flags = wire[13];
    record.reserved = readLe16(wire + 14);
    record.transaction_id = readLe32(wire + 16);
    record.target_generation = readLe32(wire + 20);
    pairReadBoard(wire + kPairRecordHeaderSize, record.left);
    pairReadBoard(wire + kPairRecordHeaderSize + kPairBoardWireSize,
                  record.right);
    pairReadTombstone(
        wire + kPairRecordHeaderSize + 2 * kPairBoardWireSize,
        record.completed);
    record.crc = readLe32(wire + kPairRecordCrcOffset);
    return validPairRecord(record);
}

inline bool pairRecordHasExactPairCommit(
    const PairTransactionRecord &record) {
    return validPairRecord(record) &&
           record.stage == PairStage::BothPairCommitted &&
           record.completed.present &&
           record.completed.outcome == snapshot::Outcome::Confirmed &&
           record.completed.transaction_id == record.transaction_id &&
           record.completed.target_generation == record.target_generation &&
           record.left.pair_committed && record.right.pair_committed &&
           record.left.local_validated && record.right.local_validated &&
           memcmp(record.completed.left_image_sha256,
                  record.left.image_sha256, 32) == 0 &&
           memcmp(record.completed.right_image_sha256,
                  record.right.image_sha256, 32) == 0 &&
           memcmp(record.completed.left_manifest_sha256,
                  record.left.manifest_sha256, 32) == 0 &&
           memcmp(record.completed.right_manifest_sha256,
                  record.right.manifest_sha256, 32) == 0 &&
           memcmp(record.completed.left_identity_digest,
                  record.left.identity_digest, 32) == 0 &&
           memcmp(record.completed.right_identity_digest,
                  record.right.identity_digest, 32) == 0;
}

inline bool pairRecordHasExactPairRollback(
    const PairTransactionRecord &record) {
    return validPairRecord(record) &&
           record.stage == PairStage::RolledBack &&
           record.completed.present &&
           record.completed.outcome == snapshot::Outcome::RolledBack &&
           record.completed.terminal_command == Command::Abort &&
           record.completed.transaction_id == record.transaction_id &&
           record.completed.target_generation == record.target_generation &&
           record.left.local_phase == snapshot::Phase::RolledBack &&
           record.right.local_phase == snapshot::Phase::RolledBack &&
           !record.left.pair_committed && !record.right.pair_committed &&
           memcmp(record.completed.left_image_sha256,
                  record.left.image_sha256, 32) == 0 &&
           memcmp(record.completed.right_image_sha256,
                  record.right.image_sha256, 32) == 0 &&
           memcmp(record.completed.left_manifest_sha256,
                  record.left.manifest_sha256, 32) == 0 &&
           memcmp(record.completed.right_manifest_sha256,
                  record.right.manifest_sha256, 32) == 0 &&
           memcmp(record.completed.left_identity_digest,
                  record.left.identity_digest, 32) == 0 &&
           memcmp(record.completed.right_identity_digest,
                  record.right.identity_digest, 32) == 0;
}

// A single board's durable proof that it rolled all the way back: it is
// running its previous slot, from its previous image, with an Abort tombstone
// naming its own transaction.  Nothing is owed to a board in this state.
inline bool exactBoardRollbackFact(const PairBoardFact &fact, Target role) {
    return validPairBoardFact(fact, role) && fact.identity_complete &&
           !fact.local_validated && !fact.pair_committed &&
           fact.local_phase == snapshot::Phase::RolledBack &&
           (fact.completed_outcome == snapshot::Outcome::Aborted ||
            fact.completed_outcome == snapshot::Outcome::RolledBack) &&
           fact.completed_terminal_command == Command::Abort &&
           fact.completed_transaction_id == fact.transaction_id &&
           fact.completed_terminal_sequence != 0 &&
           fact.completed_terminal_sequence != UINT32_MAX &&
           fact.running_slot == fact.previous_slot &&
           fact.boot_slot == fact.previous_slot &&
           memcmp(fact.running_image_sha256, fact.previous_image_sha256, 32) ==
               0;
}

// The third terminal shape, alongside an exact pair commit and an exact pair
// rollback: one board joined the transaction and durably rolled itself back,
// and the other never joined at all.  Such a record installed nothing on
// either board, so it owes no rollback and holds no evidence a later
// transaction needs -- it is exactly as retirable as a two-board rollback.
//
// Without this, a transaction the peer never joined parks at RollbackIntent
// forever: reaching RolledBack requires *both* facts to be present and rolled
// back, which an absent participant can never supply.
inline bool pairRecordHasExactOneSidedRollback(
    const PairTransactionRecord &record) {
    if (!validPairRecord(record) ||
        record.stage != PairStage::RollbackIntent)
        return false;
    if (record.left.present == record.right.present) return false;
    const PairBoardFact &joined =
        record.left.present ? record.left : record.right;
    // The property that makes this record displaceable is that the
    // transaction's image will never boot -- not that the board still runs the
    // exact bytes that transaction recorded.  exactBoardRollbackFact also
    // compares running_image_sha256 against previous_image_sha256, but the
    // first is measured live from the running partition and the second is a
    // value the transaction wrote down.  Any out-of-band write to the running
    // slot -- developer flash, factory reflash, recovery flash -- separates
    // them for good, and requiring equality here would wedge the record
    // permanently with no exit, which is the failure this predicate exists to
    // prevent.  boot_slot carries the whole safety claim.
    return joined.identity_complete && !joined.local_validated &&
           !joined.pair_committed &&
           joined.local_phase == snapshot::Phase::RolledBack &&
           (joined.completed_outcome == snapshot::Outcome::Aborted ||
            joined.completed_outcome == snapshot::Outcome::RolledBack) &&
           joined.completed_terminal_command == Command::Abort &&
           joined.completed_transaction_id == joined.transaction_id &&
           joined.completed_terminal_sequence != 0 &&
           joined.completed_terminal_sequence != UINT32_MAX &&
           joined.boot_slot != joined.target_slot &&
           joined.boot_slot == joined.previous_slot;
}

inline bool pairRecordRequiresHostReconcile(
    const PairTransactionRecord &record) {
    if (!validPairRecord(record)) return true;
    return record.stage != PairStage::None &&
           record.stage != PairStage::BothPairCommitted &&
           record.stage != PairStage::RolledBack;
}

inline RecoveryAction recoveryAction(const PairTransactionRecord &record) {
    if (!validPairRecord(record)) return RecoveryAction::EnterSafeMaintenance;
    return pairRecordRequiresHostReconcile(record)
               ? RecoveryAction::HostReconcile
               : RecoveryAction::None;
}

} // namespace makcm::ota
