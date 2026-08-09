#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "OtaProtocol.h"

namespace makcm::ota {

// Status V2 is the sole detailed status format and has one fixed wire layout.
constexpr uint8_t kStatusV2Version = 2;
constexpr size_t kStatusV2PayloadSize = 232;

constexpr size_t kStatusV2TargetImageSha256Offset = 56;
constexpr size_t kStatusV2RunningImageSha256Offset = 88;
constexpr size_t kStatusV2ManifestSha256Offset = 120;
constexpr size_t kStatusV2HardwareRevisionOffset = 152;
constexpr size_t kStatusV2ProjectNameOffset = 168;
constexpr size_t kStatusV2AppVersionOffset = 200;

struct StatusPayloadV2 {
    Target role = Target::Host;
    State state = State::Idle;
    Command last_command = Command::Hello;
    Command last_response_command = Command::StatusResponse;
    NackReason reason = NackReason::None;
    uint32_t status_flags = 0;
    uint32_t next_offset = 0;
    uint32_t transaction_id = 0;
    uint32_t target_generation = 0;
    uint32_t running_generation = 0;
    uint32_t partition_schema = 0;
    uint32_t image_size = 0;
    uint32_t reset_reason = 0;
    int32_t last_ota_error = 0;
    int32_t last_state_query_error = 0;
    uint8_t running_slot = 0xFF;
    uint8_t boot_slot = 0xFF;
    uint8_t target_slot = 0xFF;
    uint8_t image_state = 0xFF;
    uint8_t target_image_sha256[32]{};
    uint8_t running_image_sha256[32]{};
    uint8_t manifest_sha256[32]{};
    char hardware_revision[16]{};
    char project_name[32]{};
    char app_version[32]{};
};

inline bool statusV2AllZero(const uint8_t *data, size_t length) {
    uint8_t aggregate = 0;
    for (size_t i = 0; i < length; ++i) aggregate |= data[i];
    return aggregate == 0;
}

inline bool validStatusV2FixedString(const char *value, size_t capacity) {
    if (value == nullptr || capacity < 2 || value[0] == '\0') return false;
    size_t length = 0;
    while (length < capacity && value[length] != '\0') ++length;
    if (length == capacity) return false;
    for (size_t i = length + 1; i < capacity; ++i)
        if (value[i] != '\0') return false;
    return true;
}

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
        static_cast<uint8_t>(status.state) >
            static_cast<uint8_t>(State::PendingConfirm) ||
        !validCommand(status.last_command) ||
        !validResponseCommand(status.last_response_command) ||
        !validNackReason(status.reason) ||
        (status.status_flags & ~kKnownStatusFlags) != 0 ||
        status.running_generation == 0 ||
        status.partition_schema == 0 ||
        statusV2AllZero(status.running_image_sha256, 32) ||
        !validStatusV2FixedString(status.hardware_revision,
                                  sizeof(status.hardware_revision)) ||
        !validStatusV2FixedString(status.project_name,
                                  sizeof(status.project_name)) ||
        !validStatusV2FixedString(status.app_version,
                                  sizeof(status.app_version)))
        return false;

    if ((status.last_response_command == Command::Nack) !=
        (status.reason != NackReason::None))
        return false;

    const bool state_query_ok = status.last_state_query_error == 0;
    if (state_query_ok) {
        if (status.running_slot == 0xFF || status.boot_slot == 0xFF ||
            status.image_state > 5)
            return false;
    } else if (status.running_slot != 0xFF || status.boot_slot != 0xFF ||
               status.image_state != 0xFF) {
        return false;
    }

    const bool transient = status.state == State::Receiving ||
                           status.state == State::Prepared;
    if (transient) {
        if (status.transaction_id == 0 || status.target_generation == 0 ||
            status.image_size == 0 ||
            status.next_offset > status.image_size ||
            status.target_slot == 0xFF ||
            statusV2AllZero(status.target_image_sha256, 32) ||
            statusV2AllZero(status.manifest_sha256, 32))
            return false;
        if (status.state == State::Prepared &&
            status.next_offset != status.image_size)
            return false;
    } else if (status.transaction_id != 0 || status.target_generation != 0 ||
               status.image_size != 0 ||
               status.next_offset != 0 || status.target_slot != 0xFF ||
               !statusV2AllZero(status.target_image_sha256, 32) ||
               !statusV2AllZero(status.manifest_sha256, 32)) {
        // Idle has no transaction. PendingConfirm is identified solely by
        // the running generation and running image hash.
        return false;
    }
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
    wire[2] = static_cast<uint8_t>(status.state);
    wire[3] = static_cast<uint8_t>(status.last_command);
    wire[4] = static_cast<uint8_t>(status.last_response_command);
    wire[5] = status.running_slot;
    wire[6] = status.boot_slot;
    wire[7] = status.target_slot;
    wire[8] = status.image_state;
    writeLe16(wire + 12, static_cast<uint16_t>(status.reason));
    writeLe32(wire + 16, status.status_flags);
    writeLe32(wire + 20, status.next_offset);
    writeLe32(wire + 24, status.transaction_id);
    writeLe32(wire + 28, status.target_generation);
    writeLe32(wire + 32, status.running_generation);
    writeLe32(wire + 36, status.partition_schema);
    writeLe32(wire + 40, status.image_size);
    writeLe32(wire + 44, status.reset_reason);
    writeLe32(wire + 48, static_cast<uint32_t>(status.last_ota_error));
    writeLe32(wire + 52,
              static_cast<uint32_t>(status.last_state_query_error));
    memcpy(wire + kStatusV2TargetImageSha256Offset,
           status.target_image_sha256, 32);
    memcpy(wire + kStatusV2RunningImageSha256Offset,
           status.running_image_sha256, 32);
    memcpy(wire + kStatusV2ManifestSha256Offset,
           status.manifest_sha256, 32);
    memcpy(wire + kStatusV2HardwareRevisionOffset,
           status.hardware_revision, 16);
    memcpy(wire + kStatusV2ProjectNameOffset, status.project_name, 32);
    memcpy(wire + kStatusV2AppVersionOffset, status.app_version, 32);
    return true;
}

inline bool decodeStatusV2(const uint8_t *wire, size_t length,
                           StatusPayloadV2 &status) {
    if (wire == nullptr || length != kStatusV2PayloadSize ||
        wire[0] != kStatusV2Version || wire[9] != 0 || wire[10] != 0 ||
        wire[11] != 0 || wire[14] != 0 || wire[15] != 0)
        return false;

    StatusPayloadV2 decoded{};
    decoded.role = static_cast<Target>(wire[1]);
    decoded.state = static_cast<State>(wire[2]);
    decoded.last_command = static_cast<Command>(wire[3]);
    decoded.last_response_command = static_cast<Command>(wire[4]);
    decoded.running_slot = wire[5];
    decoded.boot_slot = wire[6];
    decoded.target_slot = wire[7];
    decoded.image_state = wire[8];
    decoded.reason = static_cast<NackReason>(readLe16(wire + 12));
    decoded.status_flags = readLe32(wire + 16);
    decoded.next_offset = readLe32(wire + 20);
    decoded.transaction_id = readLe32(wire + 24);
    decoded.target_generation = readLe32(wire + 28);
    decoded.running_generation = readLe32(wire + 32);
    decoded.partition_schema = readLe32(wire + 36);
    decoded.image_size = readLe32(wire + 40);
    decoded.reset_reason = readLe32(wire + 44);
    decoded.last_ota_error = static_cast<int32_t>(readLe32(wire + 48));
    decoded.last_state_query_error =
        static_cast<int32_t>(readLe32(wire + 52));
    memcpy(decoded.target_image_sha256,
           wire + kStatusV2TargetImageSha256Offset, 32);
    memcpy(decoded.running_image_sha256,
           wire + kStatusV2RunningImageSha256Offset, 32);
    memcpy(decoded.manifest_sha256, wire + kStatusV2ManifestSha256Offset, 32);
    memcpy(decoded.hardware_revision,
           wire + kStatusV2HardwareRevisionOffset, 16);
    memcpy(decoded.project_name, wire + kStatusV2ProjectNameOffset, 32);
    memcpy(decoded.app_version, wire + kStatusV2AppVersionOffset, 32);
    if (!validStatusV2(decoded)) return false;
    status = decoded;
    return true;
}

} // namespace makcm::ota
