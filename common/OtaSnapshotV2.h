#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "OtaProtocol.h"

namespace makcm::ota::snapshot {

constexpr uint32_t kMagic = 0x3253544Fu; // "OTS2"
constexpr uint16_t kSchemaVersion = 2;
constexpr uint16_t kProtocolEpoch = 2;
constexpr size_t kIdentitySize = 212;
constexpr size_t kLedgerEntrySize = 72;
constexpr size_t kLedgerCount = 5;
constexpr size_t kActiveSize = 608;
constexpr size_t kTombstoneSize = 288;
constexpr size_t kHeaderSize = 16;
constexpr size_t kSnapshotWireSize =
    kHeaderSize + kActiveSize + kTombstoneSize + 4;
constexpr size_t kActiveOffset = kHeaderSize;
constexpr size_t kTombstoneOffset = kActiveOffset + kActiveSize;
constexpr size_t kCrcOffset = kSnapshotWireSize - 4;

enum class Phase : uint8_t {
    Empty = 0,
    Receiving = 1,
    Prepared = 2,
    ActivateIntent = 3,
    BootSelected = 4,
    PendingVerify = 5,
    LocalValidated = 6,
    PairPending = 7,
    PairCommitted = 8,
    RollbackIntent = 9,
    RolledBack = 10,
    Failed = 11,
};

enum class EffectPhase : uint8_t {
    Empty = 0,
    IntentDurable = 1,
    ExternalEffectObserved = 2,
    Done = 3,
};

enum class Outcome : uint8_t {
    None = 0,
    Confirmed = 1,
    Aborted = 2,
    RolledBack = 3,
};

struct Identity {
    uint16_t protocol_epoch = kProtocolEpoch;
    Target role = Target::Host;
    uint8_t target_slot = 0xFF;
    uint8_t previous_slot = 0xFF;
    uint32_t transaction_id = 0;
    uint32_t generation = 0;
    uint32_t previous_generation = 0;
    uint32_t partition_schema = 0;
    uint32_t image_size = 0;
    uint32_t min_peer_generation = 0;
    uint32_t max_peer_generation = 0;
    char hardware_revision[16]{};
    char project_name[32]{};
    uint8_t image_sha256[32]{};
    uint8_t manifest_sha256[32]{};
    uint8_t previous_image_sha256[32]{};
    uint8_t identity_digest[32]{};
};

struct LedgerEntry {
    bool valid = false;
    Command command = Command::Hello;
    EffectPhase effect_phase = EffectPhase::Empty;
    State accepted_from_state = State::Idle;
    uint32_t sequence = 0;
    uint8_t request_sha256[32]{};
    NackReason result = NackReason::None;
    State resulting_state = State::Idle;
    uint8_t response_length = 0;
    uint32_t snapshot_commit_sequence = 0;
    uint8_t response[kStatusPayloadSize]{};
};

struct ActiveTransaction {
    bool present = false;
    Phase phase = Phase::Empty;
    bool local_validated = false;
    bool pair_committed = false;
    uint8_t running_slot = 0xFF;
    uint8_t boot_slot = 0xFF;
    uint8_t image_state = 0xFF;
    Command last_command = Command::Hello;
    uint32_t next_offset = 0;
    uint32_t last_request_sequence = 0;
    NackReason last_nack = NackReason::None;
    uint16_t boot_attempt = 0;
    uint32_t reset_reason = 0;
    int32_t last_ota_error = 0;
    int32_t last_state_error = 0;
    uint8_t last_ota_operation = 0;
    uint8_t record_error_flags = 0;
    Identity identity{};
    LedgerEntry ledger[kLedgerCount]{};
};

struct CompletedTombstone {
    bool present = false;
    Outcome outcome = Outcome::None;
    Command terminal_command = Command::Hello;
    State final_state = State::Idle;
    uint32_t terminal_sequence = 0;
    NackReason result = NackReason::None;
    uint8_t response_length = 0;
    uint8_t running_slot = 0xFF;
    uint8_t boot_slot = 0xFF;
    uint8_t image_state = 0xFF;
    bool local_validated = false;
    bool pair_committed = false;
    Identity identity{};
    uint8_t terminal_request_sha256[32]{};
    uint8_t response[kStatusPayloadSize]{};
};

struct Snapshot {
    uint32_t commit_sequence = 0;
    Target role = Target::Host;
    ActiveTransaction active{};
    CompletedTombstone completed{};
};

enum class CopySelection : int8_t {
    Initialize = -1,
    A = 0,
    B = 1,
    Fault = 2,
};

inline CopySelection selectSnapshotCopy(
    bool present_a, bool valid_a, const Snapshot &a, const uint8_t *wire_a,
    bool present_b, bool valid_b, const Snapshot &b, const uint8_t *wire_b) {
    if (!valid_a && !valid_b)
        return present_a || present_b ? CopySelection::Fault
                                      : CopySelection::Initialize;
    if (valid_a && valid_b && a.commit_sequence == b.commit_sequence) {
        if (wire_a == nullptr || wire_b == nullptr ||
            memcmp(wire_a, wire_b, kSnapshotWireSize) != 0)
            return CopySelection::Fault;
        return CopySelection::A;
    }
    if (valid_a && (!valid_b || a.commit_sequence > b.commit_sequence))
        return CopySelection::A;
    return CopySelection::B;
}

inline bool allZero(const uint8_t *data, size_t length) {
    uint8_t aggregate = 0;
    for (size_t i = 0; i < length; ++i) aggregate |= data[i];
    return aggregate == 0;
}

inline bool validFixedString(const char *value, size_t capacity) {
    if (value == nullptr || capacity < 2 || value[0] == '\0') return false;
    size_t length = 0;
    while (length < capacity && value[length] != '\0') ++length;
    if (length == capacity) return false;
    for (size_t i = length + 1; i < capacity; ++i)
        if (value[i] != '\0') return false;
    return true;
}

inline bool validPhase(Phase phase) {
    return static_cast<uint8_t>(phase) <=
           static_cast<uint8_t>(Phase::Failed);
}

inline bool validEffectPhase(EffectPhase phase) {
    return static_cast<uint8_t>(phase) <=
           static_cast<uint8_t>(EffectPhase::Done);
}

inline bool validOutcome(Outcome outcome) {
    return static_cast<uint8_t>(outcome) <=
           static_cast<uint8_t>(Outcome::RolledBack);
}

inline bool validIdentity(const Identity &identity, Target role) {
    return identity.protocol_epoch == kProtocolEpoch &&
           identity.role == role && identity.transaction_id != 0 &&
           identity.generation != 0 && identity.previous_generation != 0 &&
           identity.partition_schema != 0 && identity.image_size != 0 &&
           identity.min_peer_generation != 0 &&
           identity.min_peer_generation <= identity.generation &&
           identity.generation <= identity.max_peer_generation &&
           identity.target_slot != 0xFF && identity.previous_slot != 0xFF &&
           identity.target_slot != identity.previous_slot &&
           validFixedString(identity.hardware_revision,
                            sizeof(identity.hardware_revision)) &&
           validFixedString(identity.project_name,
                            sizeof(identity.project_name)) &&
           !allZero(identity.image_sha256, sizeof(identity.image_sha256)) &&
           !allZero(identity.manifest_sha256,
                    sizeof(identity.manifest_sha256)) &&
           !allZero(identity.previous_image_sha256,
                    sizeof(identity.previous_image_sha256)) &&
           !allZero(identity.identity_digest,
                    sizeof(identity.identity_digest));
}

inline size_t ledgerIndex(Command command) {
    switch (command) {
    case Command::Manifest: return 0;
    case Command::Prepare: return 1;
    case Command::Activate: return 2;
    case Command::Confirm: return 3;
    case Command::Abort: return 4;
    default: return kLedgerCount;
    }
}

inline bool validLedger(const LedgerEntry &entry) {
    if (!entry.valid) return true;
    return ledgerIndex(entry.command) < kLedgerCount &&
           entry.sequence != 0 && entry.sequence != UINT32_MAX &&
           validEffectPhase(entry.effect_phase) &&
           static_cast<uint8_t>(entry.accepted_from_state) <=
               static_cast<uint8_t>(State::Fault) &&
           static_cast<uint8_t>(entry.resulting_state) <=
               static_cast<uint8_t>(State::Fault) &&
           static_cast<uint16_t>(entry.result) <=
               static_cast<uint16_t>(NackReason::RollbackUnavailable) &&
           entry.response_length <= kStatusPayloadSize &&
           !allZero(entry.request_sha256, sizeof(entry.request_sha256));
}

inline bool validSnapshot(const Snapshot &snapshot) {
    if (snapshot.commit_sequence == 0 ||
        snapshot.commit_sequence == UINT32_MAX ||
        !validTarget(snapshot.role) ||
        snapshot.role == Target::Host) return false;
    if (snapshot.active.present) {
        if (!validPhase(snapshot.active.phase) ||
            snapshot.active.phase == Phase::Empty ||
            snapshot.active.phase == Phase::PairCommitted ||
            snapshot.active.pair_committed ||
            !validIdentity(snapshot.active.identity, snapshot.role))
            return false;
        const bool phase_requires_local_validation =
            snapshot.active.phase == Phase::LocalValidated ||
            snapshot.active.phase == Phase::PairPending;
        if (snapshot.active.local_validated !=
            phase_requires_local_validation)
            return false;
        uint32_t maximum_sequence = 0;
        for (size_t i = 0; i < kLedgerCount; ++i) {
            const auto &entry = snapshot.active.ledger[i];
            if (!validLedger(entry)) return false;
            if (entry.valid && ledgerIndex(entry.command) != i) return false;
            for (size_t j = 0; entry.valid && j < i; ++j)
                if (snapshot.active.ledger[j].valid &&
                    snapshot.active.ledger[j].sequence == entry.sequence)
                    return false;
            if (entry.valid && entry.sequence > maximum_sequence)
                maximum_sequence = entry.sequence;
            if (entry.valid && entry.effect_phase == EffectPhase::Done &&
                (entry.response_length != kStatusPayloadSize ||
                 entry.snapshot_commit_sequence == 0 ||
                 entry.snapshot_commit_sequence > snapshot.commit_sequence))
                return false;
            if (entry.valid && entry.effect_phase != EffectPhase::Done &&
                (entry.response_length != 0 ||
                 entry.snapshot_commit_sequence != 0))
                return false;
        }
        if (snapshot.active.last_request_sequence != maximum_sequence ||
            snapshot.active.last_request_sequence == UINT32_MAX)
            return false;
    }
    if (snapshot.completed.present) {
        if (!validOutcome(snapshot.completed.outcome) ||
            snapshot.completed.outcome == Outcome::None ||
            (snapshot.completed.terminal_command != Command::Confirm &&
             snapshot.completed.terminal_command != Command::Abort) ||
            !validIdentity(snapshot.completed.identity, snapshot.role) ||
            snapshot.completed.terminal_sequence == 0 ||
            snapshot.completed.terminal_sequence == UINT32_MAX ||
            snapshot.completed.response_length != kStatusPayloadSize ||
            allZero(snapshot.completed.terminal_request_sha256,
                    sizeof(snapshot.completed.terminal_request_sha256)))
            return false;
        if ((snapshot.completed.outcome == Outcome::Confirmed) !=
                (snapshot.completed.terminal_command == Command::Confirm) ||
            (snapshot.completed.outcome == Outcome::Confirmed &&
             (!snapshot.completed.local_validated ||
              !snapshot.completed.pair_committed ||
              snapshot.completed.result != NackReason::None ||
              snapshot.completed.final_state != State::Confirmed)) ||
            (snapshot.completed.terminal_command == Command::Abort &&
             (snapshot.completed.result != NackReason::None ||
              snapshot.completed.final_state != State::Idle ||
              snapshot.completed.pair_committed)))
            return false;
    }
    if (snapshot.active.present && snapshot.completed.present) {
        // A new transaction is always a different transaction.
        if (snapshot.active.identity.transaction_id ==
            snapshot.completed.identity.transaction_id)
            return false;
        // Generation monotonicity applies only to a tombstone that actually
        // installed something.  An Aborted/RolledBack transaction left the
        // board running the previous generation, so re-attempting that same
        // generation is a forward move, not a replay.  Gating on every
        // tombstone meant one failed attempt permanently consumed its
        // generation: every retry built a snapshot its own encoder rejected,
        // which surfaced as the opaque FlashWriteFailed and made the failure
        // look like flash trouble rather than a refused retry.
        if (snapshot.completed.outcome == Outcome::Confirmed &&
            snapshot.active.identity.generation <=
                snapshot.completed.identity.generation)
            return false;
    }
    return true;
}

inline void encodeIdentity(const Identity &identity, uint8_t *out) {
    memset(out, 0, kIdentitySize);
    writeLe16(out, identity.protocol_epoch);
    out[2] = static_cast<uint8_t>(identity.role);
    out[3] = identity.target_slot;
    out[4] = identity.previous_slot;
    writeLe32(out + 8, identity.transaction_id);
    writeLe32(out + 12, identity.generation);
    writeLe32(out + 16, identity.previous_generation);
    writeLe32(out + 20, identity.partition_schema);
    writeLe32(out + 24, identity.image_size);
    writeLe32(out + 28, identity.min_peer_generation);
    writeLe32(out + 32, identity.max_peer_generation);
    memcpy(out + 36, identity.hardware_revision, 16);
    memcpy(out + 52, identity.project_name, 32);
    memcpy(out + 84, identity.image_sha256, 32);
    memcpy(out + 116, identity.manifest_sha256, 32);
    memcpy(out + 148, identity.previous_image_sha256, 32);
    memcpy(out + 180, identity.identity_digest, 32);
}

inline void decodeIdentity(const uint8_t *wire, Identity &identity) {
    memset(&identity, 0, sizeof(identity));
    identity.protocol_epoch = readLe16(wire);
    identity.role = static_cast<Target>(wire[2]);
    identity.target_slot = wire[3];
    identity.previous_slot = wire[4];
    identity.transaction_id = readLe32(wire + 8);
    identity.generation = readLe32(wire + 12);
    identity.previous_generation = readLe32(wire + 16);
    identity.partition_schema = readLe32(wire + 20);
    identity.image_size = readLe32(wire + 24);
    identity.min_peer_generation = readLe32(wire + 28);
    identity.max_peer_generation = readLe32(wire + 32);
    memcpy(identity.hardware_revision, wire + 36, 16);
    memcpy(identity.project_name, wire + 52, 32);
    memcpy(identity.image_sha256, wire + 84, 32);
    memcpy(identity.manifest_sha256, wire + 116, 32);
    memcpy(identity.previous_image_sha256, wire + 148, 32);
    memcpy(identity.identity_digest, wire + 180, 32);
}

inline void encodeLedger(const LedgerEntry &entry, uint8_t *out) {
    memset(out, 0, kLedgerEntrySize);
    out[0] = entry.valid ? 1 : 0;
    if (!entry.valid) return;
    out[1] = static_cast<uint8_t>(entry.command);
    out[2] = static_cast<uint8_t>(entry.effect_phase);
    out[3] = static_cast<uint8_t>(entry.accepted_from_state);
    writeLe32(out + 4, entry.sequence);
    memcpy(out + 8, entry.request_sha256, 32);
    writeLe16(out + 40, static_cast<uint16_t>(entry.result));
    out[42] = static_cast<uint8_t>(entry.resulting_state);
    out[43] = entry.response_length;
    writeLe32(out + 44, entry.snapshot_commit_sequence);
    memcpy(out + 48, entry.response, kStatusPayloadSize);
}

inline void decodeLedger(const uint8_t *wire, LedgerEntry &entry) {
    memset(&entry, 0, sizeof(entry));
    entry.valid = wire[0] == 1;
    if (!entry.valid) return;
    entry.command = static_cast<Command>(wire[1]);
    entry.effect_phase = static_cast<EffectPhase>(wire[2]);
    entry.accepted_from_state = static_cast<State>(wire[3]);
    entry.sequence = readLe32(wire + 4);
    memcpy(entry.request_sha256, wire + 8, 32);
    entry.result = static_cast<NackReason>(readLe16(wire + 40));
    entry.resulting_state = static_cast<State>(wire[42]);
    entry.response_length = wire[43];
    entry.snapshot_commit_sequence = readLe32(wire + 44);
    memcpy(entry.response, wire + 48, kStatusPayloadSize);
}

inline void encodeActive(const ActiveTransaction &active, uint8_t *out) {
    memset(out, 0, kActiveSize);
    out[0] = active.present ? 1 : 0;
    if (!active.present) return;
    out[1] = static_cast<uint8_t>(active.phase);
    out[2] = active.local_validated ? 1 : 0;
    out[3] = active.pair_committed ? 1 : 0;
    out[4] = active.running_slot;
    out[5] = active.boot_slot;
    out[6] = active.image_state;
    out[7] = static_cast<uint8_t>(active.last_command);
    writeLe32(out + 8, active.next_offset);
    writeLe32(out + 12, active.last_request_sequence);
    writeLe16(out + 16, static_cast<uint16_t>(active.last_nack));
    writeLe16(out + 18, active.boot_attempt);
    writeLe32(out + 20, active.reset_reason);
    writeLe32(out + 24, static_cast<uint32_t>(active.last_ota_error));
    writeLe32(out + 28, static_cast<uint32_t>(active.last_state_error));
    out[32] = active.last_ota_operation;
    out[33] = active.record_error_flags;
    encodeIdentity(active.identity, out + 36);
    for (size_t i = 0; i < kLedgerCount; ++i)
        encodeLedger(active.ledger[i], out + 248 + i * kLedgerEntrySize);
}

inline void decodeActive(const uint8_t *wire, ActiveTransaction &active) {
    memset(&active, 0, sizeof(active));
    active.present = wire[0] == 1;
    if (!active.present) return;
    active.phase = static_cast<Phase>(wire[1]);
    active.local_validated = wire[2] == 1;
    active.pair_committed = wire[3] == 1;
    active.running_slot = wire[4];
    active.boot_slot = wire[5];
    active.image_state = wire[6];
    active.last_command = static_cast<Command>(wire[7]);
    active.next_offset = readLe32(wire + 8);
    active.last_request_sequence = readLe32(wire + 12);
    active.last_nack = static_cast<NackReason>(readLe16(wire + 16));
    active.boot_attempt = readLe16(wire + 18);
    active.reset_reason = readLe32(wire + 20);
    active.last_ota_error = static_cast<int32_t>(readLe32(wire + 24));
    active.last_state_error = static_cast<int32_t>(readLe32(wire + 28));
    active.last_ota_operation = wire[32];
    active.record_error_flags = wire[33];
    decodeIdentity(wire + 36, active.identity);
    for (size_t i = 0; i < kLedgerCount; ++i)
        decodeLedger(wire + 248 + i * kLedgerEntrySize, active.ledger[i]);
}

inline void encodeTombstone(const CompletedTombstone &completed,
                            uint8_t *out) {
    memset(out, 0, kTombstoneSize);
    out[0] = completed.present ? 1 : 0;
    if (!completed.present) return;
    out[1] = static_cast<uint8_t>(completed.outcome);
    out[2] = static_cast<uint8_t>(completed.terminal_command);
    out[3] = static_cast<uint8_t>(completed.final_state);
    writeLe32(out + 4, completed.terminal_sequence);
    writeLe16(out + 8, static_cast<uint16_t>(completed.result));
    out[10] = completed.response_length;
    out[11] = completed.running_slot;
    out[12] = completed.boot_slot;
    out[13] = completed.image_state;
    out[14] = completed.local_validated ? 1 : 0;
    out[15] = completed.pair_committed ? 1 : 0;
    encodeIdentity(completed.identity, out + 16);
    memcpy(out + 228, completed.terminal_request_sha256, 32);
    memcpy(out + 260, completed.response, kStatusPayloadSize);
}

inline void decodeTombstone(const uint8_t *wire,
                            CompletedTombstone &completed) {
    memset(&completed, 0, sizeof(completed));
    completed.present = wire[0] == 1;
    if (!completed.present) return;
    completed.outcome = static_cast<Outcome>(wire[1]);
    completed.terminal_command = static_cast<Command>(wire[2]);
    completed.final_state = static_cast<State>(wire[3]);
    completed.terminal_sequence = readLe32(wire + 4);
    completed.result = static_cast<NackReason>(readLe16(wire + 8));
    completed.response_length = wire[10];
    completed.running_slot = wire[11];
    completed.boot_slot = wire[12];
    completed.image_state = wire[13];
    completed.local_validated = wire[14] == 1;
    completed.pair_committed = wire[15] == 1;
    decodeIdentity(wire + 16, completed.identity);
    memcpy(completed.terminal_request_sha256, wire + 228, 32);
    memcpy(completed.response, wire + 260, kStatusPayloadSize);
}

inline bool encodeSnapshot(const Snapshot &snapshot, uint8_t *wire,
                           size_t capacity) {
    if (wire == nullptr || capacity < kSnapshotWireSize ||
        !validSnapshot(snapshot)) return false;
    memset(wire, 0, kSnapshotWireSize);
    writeLe32(wire, kMagic);
    writeLe16(wire + 4, kSchemaVersion);
    writeLe16(wire + 6, static_cast<uint16_t>(kSnapshotWireSize));
    writeLe32(wire + 8, snapshot.commit_sequence);
    wire[12] = static_cast<uint8_t>(snapshot.role);
    encodeActive(snapshot.active, wire + kActiveOffset);
    encodeTombstone(snapshot.completed, wire + kTombstoneOffset);
    writeLe32(wire + kCrcOffset, crc32(wire, kCrcOffset));
    return true;
}

inline bool decodeSnapshot(const uint8_t *wire, size_t length,
                           Snapshot &snapshot) {
    if (wire == nullptr || length != kSnapshotWireSize ||
        readLe32(wire) != kMagic || readLe16(wire + 4) != kSchemaVersion ||
        readLe16(wire + 6) != kSnapshotWireSize ||
        crc32(wire, kCrcOffset) != readLe32(wire + kCrcOffset))
        return false;
    if (wire[13] != 0 || wire[14] != 0 || wire[15] != 0 ||
        wire[kActiveOffset] > 1 || wire[kTombstoneOffset] > 1)
        return false;
    if (wire[kActiveOffset] == 0) {
        if (!allZero(wire + kActiveOffset + 1, kActiveSize - 1)) return false;
    } else {
        const uint8_t *active = wire + kActiveOffset;
        if (active[2] > 1 || active[3] > 1 || active[34] != 0 ||
            active[35] != 0 || active[36 + 5] != 0 ||
            active[36 + 6] != 0 || active[36 + 7] != 0)
            return false;
        for (size_t i = 0; i < kLedgerCount; ++i) {
            const uint8_t *entry = active + 248 + i * kLedgerEntrySize;
            if (entry[0] > 1) return false;
            if (entry[0] == 0 && !allZero(entry + 1, kLedgerEntrySize - 1))
                return false;
            if (entry[0] == 1) {
                const uint8_t response_length = entry[43];
                for (size_t j = response_length; j < kStatusPayloadSize; ++j)
                    if (entry[48 + j] != 0) return false;
            }
        }
    }
    if (wire[kTombstoneOffset] == 0) {
        if (!allZero(wire + kTombstoneOffset + 1, kTombstoneSize - 1))
            return false;
    } else {
        const uint8_t *completed = wire + kTombstoneOffset;
        if (completed[14] > 1 || completed[15] > 1 ||
            completed[16 + 5] != 0 || completed[16 + 6] != 0 ||
            completed[16 + 7] != 0 ||
            !allZero(completed + 284, 4))
            return false;
    }
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.commit_sequence = readLe32(wire + 8);
    snapshot.role = static_cast<Target>(wire[12]);
    decodeActive(wire + kActiveOffset, snapshot.active);
    decodeTombstone(wire + kTombstoneOffset, snapshot.completed);
    return validSnapshot(snapshot);
}

} // namespace makcm::ota::snapshot
