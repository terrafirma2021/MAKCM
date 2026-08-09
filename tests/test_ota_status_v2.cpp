#include <array>
#include <cassert>
#include <cstring>
#include <vector>

#include "OtaStatusV2.h"

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

int main() {
    static_assert(kStatusV2PayloadSize == 232);
    static_assert(static_cast<uint8_t>(State::Idle) == 0);
    static_assert(static_cast<uint8_t>(State::Receiving) == 1);
    static_assert(static_cast<uint8_t>(State::Prepared) == 2);
    static_assert(static_cast<uint8_t>(State::PendingConfirm) == 3);

    StatusPayloadV2 source{};
    source.role = Target::Left;
    source.state = State::Prepared;
    source.last_command = Command::Prepare;
    source.last_response_command = Command::Ack;
    source.status_flags = kStatusRollbackSupported |
                          kStatusTrustConfigured |
                          kStatusMaintenanceExclusive;
    source.next_offset = 4096;
    source.transaction_id = 0x11223344;
    source.target_generation = 14;
    source.running_generation = 13;
    source.partition_schema = 0x4C010001;
    source.image_size = 4096;
    source.reset_reason = 3;
    source.last_ota_error = -4;
    source.running_slot = 0x10;
    source.boot_slot = 0x10;
    source.target_slot = 0x11;
    source.image_state = 3;
    std::memset(source.target_image_sha256, 0x11, 32);
    std::memset(source.running_image_sha256, 0x22, 32);
    std::memset(source.manifest_sha256, 0x33, 32);
    std::memcpy(source.hardware_revision, "makcm-s3-r1", 12);
    std::memcpy(source.project_name, "makcm_left_idf", 15);
    std::memcpy(source.app_version, "makcm-g14", 10);

    const auto golden = fromHex(
        "020102058010101103000000000000000b00000000100000443322110e000000"
        "0d0000000100014c0010000003000000fcffffff000000001111111111111111"
        "1111111111111111111111111111111111111111111111112222222222222222"
        "2222222222222222222222222222222222222222222222223333333333333333"
        "3333333333333333333333333333333333333333333333336d616b636d2d7333"
        "2d723100000000006d616b636d5f6c6566745f69646600000000000000000000"
        "00000000000000006d616b636d2d673134000000000000000000000000000000"
        "0000000000000000");
    assert(golden.size() == kStatusV2PayloadSize);
    std::array<uint8_t, kStatusV2PayloadSize> wire{};
    assert(encodeStatusV2(source, wire.data(), wire.size()));
    assert(std::memcmp(wire.data(), golden.data(), golden.size()) == 0);

    StatusPayloadV2 decoded{};
    assert(decodeStatusV2(golden.data(), golden.size(), decoded));
    assert(decoded.role == Target::Left);
    assert(decoded.state == State::Prepared);
    assert(decoded.transaction_id == 0x11223344);
    assert(decoded.running_generation == 13);
    assert(decoded.last_ota_error == -4);
    assert(std::memcmp(decoded.target_image_sha256,
                       source.target_image_sha256, 32) == 0);
    assert(std::strcmp(decoded.app_version, "makcm-g14") == 0);

    std::array<uint8_t, 400> retired_wire{};
    std::memcpy(retired_wire.data(), golden.data(), golden.size());
    assert(!decodeStatusV2(retired_wire.data(), retired_wire.size(), decoded));

    auto bad = wire;
    bad[9] = 1;
    assert(!decodeStatusV2(bad.data(), bad.size(), decoded));
    bad = wire;
    bad[14] = 1;
    assert(!decodeStatusV2(bad.data(), bad.size(), decoded));
    bad = wire;
    bad[2] = 4;
    assert(!decodeStatusV2(bad.data(), bad.size(), decoded));
    bad = wire;
    bad[1] = 3; // Retired Pair target.
    assert(!decodeStatusV2(bad.data(), bad.size(), decoded));
    bad = wire;
    writeLe32(bad.data() + 16, kKnownStatusFlags | 0x80000000u);
    assert(!decodeStatusV2(bad.data(), bad.size(), decoded));
    bad = wire;
    bad[kStatusV2HardwareRevisionOffset + 15] = 1;
    assert(!decodeStatusV2(bad.data(), bad.size(), decoded));

    StatusPayloadV2 idle{};
    idle.role = Target::Right;
    idle.running_generation = 9;
    idle.partition_schema = 0x52010001;
    idle.running_slot = 0x11;
    idle.boot_slot = 0x11;
    idle.image_state = 3;
    std::memset(idle.running_image_sha256, 0x44, 32);
    std::memcpy(idle.hardware_revision, "makcm-s3-r1", 12);
    std::memcpy(idle.project_name, "makcm_right", 12);
    std::memcpy(idle.app_version, "makcm-g9", 9);
    assert(validStatusV2(idle));
    idle.transaction_id = 1;
    assert(!validStatusV2(idle));

    StatusPayloadV2 pending = idle;
    pending.transaction_id = 0;
    pending.state = State::PendingConfirm;
    pending.status_flags = kStatusLocalBootValidated;
    assert(validStatusV2(pending));
    pending.running_generation = 0;
    assert(!validStatusV2(pending));

    StatusPayloadV2 receiving = idle;
    receiving.state = State::Receiving;
    assert(!validStatusV2(receiving));

    return 0;
}
