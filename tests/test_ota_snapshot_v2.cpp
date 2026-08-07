#include <array>
#include <cassert>
#include <cstring>

#include "OtaSnapshotV2.h"

using namespace makcm::ota;
using namespace makcm::ota::snapshot;

namespace {

Snapshot makeSnapshot(uint32_t commit_sequence = 7) {
    Snapshot snapshot{};
    snapshot.commit_sequence = commit_sequence;
    snapshot.role = Target::Left;
    snapshot.active.present = true;
    snapshot.active.phase = Phase::BootSelected;
    snapshot.active.running_slot = 0x10;
    snapshot.active.boot_slot = 0x11;
    snapshot.active.image_state = 1;
    snapshot.active.last_command = Command::Activate;
    snapshot.active.last_request_sequence = 13;
    snapshot.active.identity.protocol_epoch = kProtocolEpoch;
    snapshot.active.identity.role = Target::Left;
    snapshot.active.identity.target_slot = 0x11;
    snapshot.active.identity.previous_slot = 0x10;
    snapshot.active.identity.transaction_id = 0x12345678;
    snapshot.active.identity.generation = 9;
    snapshot.active.identity.previous_generation = 8;
    snapshot.active.identity.partition_schema = 0x4C010001;
    snapshot.active.identity.image_size = 0x54321;
    snapshot.active.identity.min_peer_generation = 8;
    snapshot.active.identity.max_peer_generation = 9;
    std::memcpy(snapshot.active.identity.hardware_revision, "makcm-s3-r1", 12);
    std::memcpy(snapshot.active.identity.project_name, "makcm_left_idf", 15);
    std::memset(snapshot.active.identity.image_sha256, 0x11, 32);
    std::memset(snapshot.active.identity.manifest_sha256, 0x22, 32);
    std::memset(snapshot.active.identity.previous_image_sha256, 0x23, 32);
    std::memset(snapshot.active.identity.identity_digest, 0x33, 32);

    auto &entry = snapshot.active.ledger[ledgerIndex(Command::Activate)];
    entry.valid = true;
    entry.command = Command::Activate;
    entry.effect_phase = EffectPhase::Done;
    entry.accepted_from_state = State::Prepared;
    entry.sequence = 13;
    std::memset(entry.request_sha256, 0x44, 32);
    entry.result = NackReason::None;
    entry.resulting_state = State::ActivationPending;
    entry.response_length = kStatusPayloadSize;
    entry.snapshot_commit_sequence = commit_sequence;
    std::memset(entry.response, 0x55, kStatusPayloadSize);
    return snapshot;
}

} // namespace

int main() {
    static_assert(kSnapshotWireSize == 916);
    static_assert(makcm::ota::snapshot::kHeaderSize + kActiveSize +
                      kTombstoneSize + 4 ==
                  kSnapshotWireSize);

    Snapshot source = makeSnapshot();
    std::array<uint8_t, kSnapshotWireSize> wire{};
    assert(encodeSnapshot(source, wire.data(), wire.size()));
    assert(readLe32(wire.data()) == makcm::ota::snapshot::kMagic);
    assert(readLe16(wire.data() + 4) == kSchemaVersion);
    assert(readLe16(wire.data() + 6) == kSnapshotWireSize);
    assert(readLe32(wire.data() + 8) == source.commit_sequence);
    assert(readLe32(wire.data() + kCrcOffset) ==
           crc32(wire.data(), kCrcOffset));

    Snapshot decoded{};
    assert(decodeSnapshot(wire.data(), wire.size(), decoded));
    assert(decoded.commit_sequence == source.commit_sequence);
    assert(decoded.active.identity.transaction_id == 0x12345678);
    assert(decoded.active.identity.target_slot == 0x11);
    const auto &decoded_entry =
        decoded.active.ledger[ledgerIndex(Command::Activate)];
    assert(decoded_entry.effect_phase == EffectPhase::Done);
    assert(decoded_entry.sequence == 13);
    assert(decoded_entry.response_length == kStatusPayloadSize);

    auto damaged = wire;
    damaged[kActiveOffset + 40] ^= 0x80;
    assert(!decodeSnapshot(damaged.data(), damaged.size(), decoded));
    assert(!decodeSnapshot(wire.data(), wire.size() - 1, decoded));

    Snapshot older = source;
    older.commit_sequence = 6;
    older.active.ledger[ledgerIndex(Command::Activate)]
        .snapshot_commit_sequence = 6;
    std::array<uint8_t, kSnapshotWireSize> older_wire{};
    assert(encodeSnapshot(older, older_wire.data(), older_wire.size()));
    assert(selectSnapshotCopy(true, true, source, wire.data(),
                              true, true, older, older_wire.data()) ==
           CopySelection::A);
    assert(selectSnapshotCopy(true, false, source, damaged.data(),
                              true, true, older, older_wire.data()) ==
           CopySelection::B);
    assert(selectSnapshotCopy(true, false, source, damaged.data(),
                              true, false, older, damaged.data()) ==
           CopySelection::Fault);
    assert(selectSnapshotCopy(false, false, source, nullptr,
                              false, false, older, nullptr) ==
           CopySelection::Initialize);
    Snapshot same_sequence = source;
    auto different_wire = wire;
    different_wire[kActiveOffset + 4] ^= 1;
    assert(selectSnapshotCopy(true, true, source, wire.data(), true, true,
                              same_sequence, different_wire.data()) ==
           CopySelection::Fault);

    Snapshot invalid = source;
    invalid.commit_sequence = 0;
    assert(!encodeSnapshot(invalid, wire.data(), wire.size()));
    invalid = source;
    invalid.active.identity.transaction_id = 0;
    assert(!encodeSnapshot(invalid, wire.data(), wire.size()));
    invalid = source;
    invalid.active.ledger[ledgerIndex(Command::Activate)].sequence = 0;
    assert(!encodeSnapshot(invalid, wire.data(), wire.size()));
    invalid = source;
    invalid.active.ledger[ledgerIndex(Command::Activate)].response_length = 0;
    assert(!encodeSnapshot(invalid, wire.data(), wire.size()));
    invalid = source;
    invalid.active.ledger[ledgerIndex(Command::Activate)]
        .snapshot_commit_sequence = source.commit_sequence + 1;
    assert(!encodeSnapshot(invalid, wire.data(), wire.size()));

    Snapshot completed = source;
    completed.active = {};
    completed.completed.present = true;
    completed.completed.outcome = Outcome::Confirmed;
    completed.completed.terminal_command = Command::Confirm;
    completed.completed.final_state = State::Confirmed;
    completed.completed.terminal_sequence = 21;
    completed.completed.response_length = kStatusPayloadSize;
    completed.completed.running_slot = 0x11;
    completed.completed.boot_slot = 0x11;
    completed.completed.image_state = 2;
    completed.completed.local_validated = true;
    completed.completed.pair_committed = true;
    completed.completed.identity = source.active.identity;
    std::memset(completed.completed.terminal_request_sha256, 0x66, 32);
    std::memset(completed.completed.response, 0x77, kStatusPayloadSize);
    assert(encodeSnapshot(completed, wire.data(), wire.size()));
    assert(decodeSnapshot(wire.data(), wire.size(), decoded));
    assert(decoded.completed.outcome == Outcome::Confirmed);
    assert(decoded.completed.pair_committed);

    completed.completed.terminal_command = Command::Prepare;
    assert(!encodeSnapshot(completed, wire.data(), wire.size()));
    completed.completed.terminal_command = Command::Confirm;
    completed.completed.pair_committed = false;
    assert(!encodeSnapshot(completed, wire.data(), wire.size()));
    completed.completed.pair_committed = true;
    completed.completed.response_length = 0;
    assert(!encodeSnapshot(completed, wire.data(), wire.size()));

    return 0;
}
