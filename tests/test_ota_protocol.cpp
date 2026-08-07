#include <cassert>
#include <cstring>
#include <vector>

#include "OtaManifest.h"
#include "OtaBoardConfig.h"
#include "OtaProtocol.h"
#include "PairOtaTransaction.h"
#include "ota_protocol_golden.inc"

using namespace makcm::ota;

static std::vector<uint8_t> fromHex(const char *hex) {
    std::vector<uint8_t> result;
    const auto nibble = [](char value) -> uint8_t {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        assert(false);
        return 0;
    };
    for (size_t i = 0; hex[i] != '\0'; i += 2) {
        assert(hex[i + 1] != '\0');
        result.push_back(static_cast<uint8_t>((nibble(hex[i]) << 4) |
                                              nibble(hex[i + 1])));
    }
    return result;
}

static_assert(sizeof(makcm::ota::board::kHardwareRevisionR1) <= 16,
              "hardware revision must fit the signed manifest field");
static_assert(sizeof(makcm::ota::board::kRightManifestProjectName) <= 32,
              "right project name must fit the signed manifest field");

int main() {
    Frame golden_source{};
    golden_source.target = Target::Right;
    golden_source.command = Command::Data;
    golden_source.flags = kFlagRetransmit;
    golden_source.transaction_id = 0x12345678;
    golden_source.offset = 0x10203040;
    golden_source.sequence = 0x01020304;
    golden_source.length = 5;
    const uint8_t golden_payload[] = {0x00, 0x01, 0x7f, 0x80, 0xff};
    std::memcpy(golden_source.payload, golden_payload, sizeof(golden_payload));
    const auto golden_wire = fromHex(MAKCM_OTA_GOLDEN_FRAME_HEX);
    std::vector<uint8_t> golden_encoded(kMaxWireSize);
    const size_t golden_length = encodeFrame(
        golden_source, golden_encoded.data(), golden_encoded.size());
    assert(golden_length == golden_wire.size());
    assert(std::memcmp(golden_encoded.data(), golden_wire.data(),
                       golden_wire.size()) == 0);
    Frame golden_decoded{};
    assert(decodeFrame(golden_wire.data(), golden_wire.size(), golden_decoded));
    assert(golden_decoded.sequence == golden_source.sequence);
    assert(std::memcmp(golden_decoded.payload, golden_payload,
                       sizeof(golden_payload)) == 0);

    Frame source{};
    source.target = Target::Right;
    source.command = Command::Data;
    source.flags = kFlagRetransmit;
    source.transaction_id = 0x12345678;
    source.offset = 4096;
    source.sequence = 7;
    source.length = kMaxChunkSize;
    for (size_t i = 0; i < source.length; ++i)
        source.payload[i] = static_cast<uint8_t>(i * 17u);

    std::vector<uint8_t> wire(kMaxWireSize);
    const size_t wire_length = encodeFrame(source, wire.data(), wire.size());
    assert(wire_length == kMaxWireSize);
    Frame decoded{};
    assert(decodeFrame(wire.data(), wire_length, decoded));
    assert(decoded.target == Target::Right);
    assert(decoded.command == Command::Data);
    assert(decoded.transaction_id == source.transaction_id);
    assert(decoded.offset == source.offset && decoded.sequence == source.sequence);
    assert(decoded.length == source.length);
    assert(memcmp(decoded.payload, source.payload, source.length) == 0);

    wire[kHeaderSize + 11] ^= 0x40;
    assert(!decodeFrame(wire.data(), wire_length, decoded));
    wire[kHeaderSize + 11] ^= 0x40;

    StreamDecoder stream;
    assert(stream.feed(0x00, decoded) == FeedResult::None);
    assert(stream.feed('M', decoded) == FeedResult::None);
    assert(stream.feed('M', decoded) == FeedResult::None); // resync MMKOT
    for (size_t i = 1; i < wire_length; ++i) {
        const auto result = stream.feed(wire[i], decoded);
        assert(result == (i + 1 == wire_length ? FeedResult::FrameReady
                                               : FeedResult::None));
    }

    uint8_t manifest_wire[kManifestWireSize]{};
    writeLe16(manifest_wire, kManifestVersion);
    manifest_wire[2] = static_cast<uint8_t>(Target::Left);
    manifest_wire[3] = kImageKindApplication;
    writeLe16(manifest_wire + 4, 1);
    writeLe16(manifest_wire + 6, kEsp32S3ChipId);
    writeLe32(manifest_wire + 8, 4);
    writeLe32(manifest_wire + 12, 3);
    writeLe32(manifest_wire + 16, 4);
    writeLe32(manifest_wire + 20, 0xAABBCCDD);
    writeLe32(manifest_wire + 24, 340000);
    writeLe16(manifest_wire + 28, 4096);
    manifest_wire[30] = kSignatureEcdsaP256Sha256;
    memcpy(manifest_wire + 32, "makcm-s3-r1", 12);
    memcpy(manifest_wire + 48, "makcm_left_idf", 15);
    Manifest manifest{};
    assert(decodeManifest(manifest_wire, sizeof(manifest_wire), manifest));
    assert(manifestCompatibleWithPeer(manifest, 3));
    assert(!manifestCompatibleWithPeer(manifest, 2));
    assert(makcm::ota::board::kLeftPartitionSchemaV1 !=
           makcm::ota::board::kRightPartitionSchemaV1);
    manifest_wire[31] = 1;
    assert(!decodeManifest(manifest_wire, sizeof(manifest_wire), manifest));

    auto fill_fact = [](PairBoardFact &fact, Target role, uint32_t seq) {
        fact.present = true;
        fact.identity_complete = true;
        fact.local_validated = true;
        fact.role = role;
        fact.local_phase = snapshot::Phase::LocalValidated;
        fact.image_state = 2; // ESP_OTA_IMG_VALID
        fact.running_slot = role == Target::Left ? 0x10 : 0x11;
        fact.boot_slot = fact.running_slot;
        fact.target_slot = fact.running_slot;
        fact.previous_slot = 0x12;
        fact.last_command = Command::Prepare;
        fact.last_response_command = Command::Ack;
        fact.protocol_epoch = snapshot::kProtocolEpoch;
        fact.transaction_id = 9;
        fact.target_generation = 4;
        fact.previous_generation = 3;
        fact.partition_schema = role == Target::Left
                                    ? board::kLeftPartitionSchemaV1
                                    : board::kRightPartitionSchemaV1;
        fact.image_size = 340000;
        fact.record_sequence = seq;
        fact.last_request_sequence = 7;
        std::memset(fact.image_sha256, role == Target::Left ? 0x11 : 0x22, 32);
        std::memset(fact.manifest_sha256, role == Target::Left ? 0x33 : 0x44,
                    32);
        std::memset(fact.identity_digest, role == Target::Left ? 0x55 : 0x66,
                    32);
        std::memset(fact.running_image_sha256,
                    role == Target::Left ? 0x77 : 0x88, 32);
        std::memset(fact.previous_image_sha256,
                    role == Target::Left ? 0x99 : 0xAA, 32);
        std::strcpy(fact.hardware_revision, "makcm-s3-r1");
        std::strcpy(fact.project_name,
                    role == Target::Left ? "makcm_left_idf"
                                         : "makcm_right_arduino");
        std::strcpy(fact.app_version, role == Target::Left ? "makcm-g4"
                                                            : "makcm-g4");
    };

    PairTransactionRecord record{};
    record.stage = PairStage::BothLocalValidated;
    record.record_sequence = 1;
    record.transaction_id = 9;
    record.target_generation = 4;
    fill_fact(record.left, Target::Left, 2);
    fill_fact(record.right, Target::Right, 5);
    sealPairRecord(record);
    assert(validPairRecord(record));
    std::vector<uint8_t> pair_wire(kPairRecordWireSize);
    assert(encodePairRecord(record, pair_wire.data(), pair_wire.size()));
    PairTransactionRecord decoded_pair{};
    assert(decodePairRecord(pair_wire.data(), pair_wire.size(), decoded_pair));
    assert(decoded_pair.record_sequence == record.record_sequence);
    assert(decoded_pair.left.target_generation == 4);
    assert(decoded_pair.right.partition_schema ==
           board::kRightPartitionSchemaV1);
    assert(recoveryAction(record) == RecoveryAction::HostReconcile);

    PairCompletedTombstone completed{};
    completed.present = true;
    completed.outcome = snapshot::Outcome::Confirmed;
    completed.terminal_command = Command::Confirm;
    completed.transaction_id = 9;
    completed.target_generation = 4;
    completed.terminal_sequence = 12;
    completed.record_sequence = 3;
    completed.left_target_slot = record.left.target_slot;
    completed.right_target_slot = record.right.target_slot;
    std::memcpy(completed.left_image_sha256, record.left.image_sha256, 32);
    std::memcpy(completed.right_image_sha256, record.right.image_sha256, 32);
    std::memcpy(completed.left_manifest_sha256, record.left.manifest_sha256,
                32);
    std::memcpy(completed.right_manifest_sha256, record.right.manifest_sha256,
                32);
    std::memcpy(completed.left_identity_digest, record.left.identity_digest, 32);
    std::memcpy(completed.right_identity_digest, record.right.identity_digest,
                32);
    record.left.pair_committed = true;
    record.right.pair_committed = true;
    record.left.completed_transaction_id = 9;
    record.right.completed_transaction_id = 9;
    record.left.completed_outcome = snapshot::Outcome::Confirmed;
    record.right.completed_outcome = snapshot::Outcome::Confirmed;
    record.left.completed_terminal_command = Command::Confirm;
    record.right.completed_terminal_command = Command::Confirm;
    record.left.completed_terminal_sequence = 12;
    record.right.completed_terminal_sequence = 12;
    record.left.local_phase = snapshot::Phase::PairCommitted;
    record.right.local_phase = snapshot::Phase::PairCommitted;
    record.completed = completed;
    record.stage = PairStage::BothPairCommitted;
    sealPairRecord(record);
    assert(validPairRecord(record));
    assert(pairRecordHasExactPairCommit(record));
    assert(recoveryAction(record) == RecoveryAction::None);

    PairTransactionRecord conflict = record;
    conflict.right.image_sha256[0] ^= 1;
    sealPairRecord(conflict);
    assert(validPairRecord(conflict));
    assert(!pairRecordHasExactPairCommit(conflict));
    conflict.crc ^= 1;
    assert(!validPairRecord(conflict));

    return 0;
}
