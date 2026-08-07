#include <array>
#include <cassert>
#include <cstring>

#include "OtaStatusV2.h"

using namespace makcm::ota;

int main() {
    static_assert(kStatusV2PayloadSize == 400);
    StatusPayloadV2 source{};
    source.role = Target::Left;
    source.local_phase = snapshot::Phase::LocalValidated;
    source.state = State::PendingConfirm;
    source.last_command = Command::Status;
    source.last_response_command = Command::StatusResponse;
    source.last_ota_operation = 7;
    source.reason = NackReason::None;
    source.status_flags = kStatusLocalBootValidated |
                          kStatusMaintenanceExclusive;
    source.next_offset = 1234;
    source.record_transaction_id = 0x11223344;
    source.target_generation = 14;
    source.active_generation = 14;
    source.previous_generation = 13;
    source.partition_schema = 0x4C010001;
    source.image_size = 449312;
    source.record_sequence = 9;
    source.last_request_sequence = 27;
    source.reset_reason = 3;
    source.last_ota_error = -4;
    source.last_state_query_error = 0;
    source.boot_attempt = 2;
    source.evidence_flags = kEvidenceActivePresent |
                            kEvidenceSnapshotValid |
                            kEvidenceLocalValidated |
                            kEvidenceRunningImageHashValid |
                            kEvidenceIdentityDigestValid;
    source.running_slot = 0x11;
    source.boot_slot = 0x11;
    source.target_slot = 0x11;
    source.previous_slot = 0x10;
    source.image_state = 2;
    std::memset(source.image_sha256, 0x11, 32);
    std::memset(source.running_image_sha256, 0x11, 32);
    std::memset(source.manifest_sha256, 0x22, 32);
    std::memset(source.identity_digest, 0x33, 32);
    std::memset(source.previous_image_sha256, 0x44, 32);
    std::memset(source.app_elf_sha256, 0x55, 32);
    std::memcpy(source.hardware_revision, "makcm-s3-r1", 12);
    std::memcpy(source.project_name, "makcm_left_idf", 15);
    std::memcpy(source.app_version, "makcm-g14", 10);

    std::array<uint8_t, kStatusV2PayloadSize> wire{};
    assert(encodeStatusV2(source, wire.data(), wire.size()));
    assert(wire[0] == kStatusV2Version);
    assert(readLe32(wire.data() + 20) == 0x11223344);
    assert(readLe32(wire.data() + 48) == 9);
    StatusPayloadV2 decoded{};
    assert(decodeStatusV2(wire.data(), wire.size(), decoded));
    assert(decoded.role == Target::Left);
    assert(decoded.local_phase == snapshot::Phase::LocalValidated);
    assert(decoded.record_transaction_id == source.record_transaction_id);
    assert(decoded.previous_generation == 13);
    assert(decoded.last_ota_error == -4);
    assert(std::memcmp(decoded.image_sha256, source.image_sha256, 32) == 0);
    assert(std::strcmp(decoded.app_version, "makcm-g14") == 0);

    auto bad = wire;
    bad[82] = 1;
    assert(!decodeStatusV2(bad.data(), bad.size(), decoded));
    bad = wire;
    bad[90] = 1;
    assert(!decodeStatusV2(bad.data(), bad.size(), decoded));
    bad = wire;
    bad[1] = static_cast<uint8_t>(Target::Host);
    assert(!decodeStatusV2(bad.data(), bad.size(), decoded));

    Frame frame{};
    frame.target = Target::Left;
    frame.command = Command::Status;
    frame.flags = kFlagStatusV2;
    frame.transaction_id = 1;
    frame.sequence = 1;
    std::array<uint8_t, kMaxWireSize> frame_wire{};
    assert(encodeFrame(frame, frame_wire.data(), frame_wire.size()) != 0);
    frame.sequence = 0;
    assert(encodeFrame(frame, frame_wire.data(), frame_wire.size()) == 0);
    frame.sequence = UINT32_MAX;
    assert(encodeFrame(frame, frame_wire.data(), frame_wire.size()) == 0);
    frame.sequence = 1;
    frame.flags = 0x80;
    assert(encodeFrame(frame, frame_wire.data(), frame_wire.size()) == 0);

    return 0;
}
