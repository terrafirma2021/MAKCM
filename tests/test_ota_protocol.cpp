#include <cassert>
#include <cstring>
#include <vector>

#include "OtaManifest.h"
#include "OtaBoardConfig.h"
#include "OtaProtocol.h"
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
static_assert(sizeof(makcm::ota::board::kRightProjectName) <= 32,
              "right project name must fit the signed manifest field");

int main() {
    static_assert(static_cast<uint8_t>(State::Idle) == 0);
    static_assert(static_cast<uint8_t>(State::Receiving) == 1);
    static_assert(static_cast<uint8_t>(State::Prepared) == 2);
    static_assert(static_cast<uint8_t>(State::PendingConfirm) == 3);
    static_assert(static_cast<uint8_t>(Command::ExitMaintenance) == 0x0A);

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

    Frame retired_pair = golden_source;
    retired_pair.target = static_cast<Target>(3);
    assert(encodeFrame(retired_pair, golden_encoded.data(),
                       golden_encoded.size()) == 0);
    auto retired_pair_wire = golden_wire;
    retired_pair_wire[5] = 3;
    writeLe32(retired_pair_wire.data() + retired_pair_wire.size() - kCrcSize,
              crc32(retired_pair_wire.data() + 4,
                    retired_pair_wire.size() - 4 - kCrcSize));
    assert(!decodeFrame(retired_pair_wire.data(), retired_pair_wire.size(),
                        golden_decoded));

    const auto assert_invalid_flags = [&](Command command, uint8_t flags) {
        Frame invalid = golden_source;
        invalid.command = command;
        invalid.flags = flags;
        assert(!validFrameFlags(command, flags));
        assert(encodeFrame(invalid, golden_encoded.data(),
                           golden_encoded.size()) == 0);

        auto invalid_wire = golden_wire;
        invalid_wire[6] = static_cast<uint8_t>(command);
        invalid_wire[7] = flags;
        writeLe32(invalid_wire.data() + invalid_wire.size() - kCrcSize,
                  crc32(invalid_wire.data() + 4,
                        invalid_wire.size() - 4 - kCrcSize));
        assert(!decodeFrame(invalid_wire.data(), invalid_wire.size(),
                            golden_decoded));
    };

    for (const Command command : {
             Command::Hello, Command::EnterMaintenance, Command::Manifest,
             Command::Prepare, Command::Activate, Command::Confirm,
             Command::Abort, Command::ExitMaintenance}) {
        assert(validFrameFlags(command, 0));
        assert(validFrameFlags(command, kFlagRetransmit));
        assert_invalid_flags(command, kFlagResponse);
        assert_invalid_flags(command, kFlagFinal);
    }
    for (const uint8_t flags : {
             uint8_t{0}, uint8_t{kFlagFinal}, uint8_t{kFlagRetransmit},
             uint8_t{kFlagFinal | kFlagRetransmit}})
        assert(validFrameFlags(Command::Data, flags));
    assert_invalid_flags(Command::Data, kFlagStatusV2);
    assert_invalid_flags(Command::Data, kFlagResponse);

    assert(validFrameFlags(Command::Status, kFlagStatusV2));
    assert(validFrameFlags(Command::Status,
                           kFlagStatusV2 | kFlagRetransmit));
    assert_invalid_flags(Command::Status, 0);
    assert_invalid_flags(Command::Status, kFlagRetransmit);
    assert_invalid_flags(Command::Status, kFlagResponse | kFlagStatusV2);

    assert(validFrameFlags(Command::Ack, kFlagResponse));
    assert(validFrameFlags(Command::Nack, kFlagResponse));
    assert_invalid_flags(Command::Ack, 0);
    assert_invalid_flags(Command::Nack,
                         kFlagResponse | kFlagRetransmit);
    assert(validFrameFlags(Command::StatusResponse,
                           kFlagResponse | kFlagStatusV2));
    assert_invalid_flags(Command::StatusResponse, kFlagResponse);
    assert_invalid_flags(Command::StatusResponse,
                         kFlagResponse | kFlagStatusV2 | kFlagRetransmit);
    assert_invalid_flags(Command::Hello, 0x80);

    CommandResponsePayload ack{};
    ack.state = State::Prepared;
    ack.last_command = Command::Prepare;
    ack.next_offset = 0x10203040;
    ack.target_generation = 14;
    ack.active_generation = 13;
    ack.status_flags = kStatusTrustConfigured;
    uint8_t ack_wire[kCommandResponsePayloadSize]{};
    encodeCommandResponsePayload(ack, ack_wire);
    const auto ack_golden = fromHex(
        "0102050000000000403020100e0000000d00000002000000");
    assert(ack_golden.size() == kCommandResponsePayloadSize);
    assert(std::memcmp(ack_wire, ack_golden.data(), ack_golden.size()) == 0);
    CommandResponsePayload decoded_response{};
    assert(decodeCommandResponsePayload(ack_golden.data(), ack_golden.size(),
                                        decoded_response));
    assert(decoded_response.state == State::Prepared);

    CommandResponsePayload nack{};
    nack.state = State::Receiving;
    nack.last_command = Command::ExitMaintenance;
    nack.reason = NackReason::InvalidState;
    nack.next_offset = 0x11223344;
    nack.target_generation = 9;
    nack.active_generation = 8;
    nack.status_flags = kStatusMaintenanceExclusive;
    uint8_t nack_wire[kCommandResponsePayloadSize]{};
    encodeCommandResponsePayload(nack, nack_wire);
    const auto nack_golden = fromHex(
        "01010a000e00000044332211090000000800000008000000");
    assert(nack_golden.size() == kCommandResponsePayloadSize);
    assert(std::memcmp(nack_wire, nack_golden.data(), nack_golden.size()) == 0);
    assert(decodeCommandResponsePayload(nack_golden.data(), nack_golden.size(),
                                        decoded_response));
    assert(decoded_response.reason == NackReason::InvalidState);

    auto invalid_compact = ack_golden;
    invalid_compact[1] = 4;
    assert(!decodeCommandResponsePayload(invalid_compact.data(),
                                         invalid_compact.size(),
                                         decoded_response));

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

    return 0;
}
