#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace makcm::ota {

constexpr uint8_t kWireVersion = 1;
constexpr uint8_t kMagic[4] = {'M', 'K', 'O', 'T'};
constexpr size_t kHeaderSize = 24;
constexpr size_t kCrcSize = 4;
constexpr uint16_t kMinChunkSize = 2048;
constexpr uint16_t kMaxChunkSize = 4096;
constexpr size_t kMaxWireSize = kHeaderSize + kMaxChunkSize + kCrcSize;

enum class Target : uint8_t {
    Host = 0,
    Left = 1,
    Right = 2,
};

enum class Command : uint8_t {
    Hello = 0x01,
    EnterMaintenance = 0x02,
    Manifest = 0x03,
    Data = 0x04,
    Prepare = 0x05,
    Status = 0x06,
    Activate = 0x07,
    Confirm = 0x08,
    Abort = 0x09,
    ExitMaintenance = 0x0A,
    Ack = 0x80,
    Nack = 0x81,
    StatusResponse = 0x82,
};

enum FrameFlags : uint8_t {
    kFlagResponse = 0x01,
    kFlagRetransmit = 0x02,
    kFlagFinal = 0x04,
    // Negotiates the sole fixed, explicit detailed status payload. It is
    // valid only on STATUS requests and STATUS_RESPONSE replies.
    kFlagStatusV2 = 0x08,
};
constexpr uint8_t kKnownFrameFlags = kFlagResponse | kFlagRetransmit |
                                     kFlagFinal | kFlagStatusV2;

enum class NackReason : uint16_t {
    None = 0,
    Malformed = 1,
    CrcMismatch = 2,
    UnsupportedVersion = 3,
    WrongTarget = 4,
    WrongTransaction = 5,
    NotInMaintenance = 6,
    Busy = 7,
    ManifestRejected = 8,
    WrongOffset = 9,
    FlashWriteFailed = 10,
    ImageVerificationFailed = 11,
    SignatureVerificationFailed = 12,
    CompatibilityRejected = 13,
    InvalidState = 14,
    Timeout = 15,
    RollbackUnavailable = 16,
};

enum class State : uint8_t {
    Idle = 0,
    Receiving = 1,
    Prepared = 2,
    PendingConfirm = 3,
};

enum StatusFlags : uint32_t {
    kStatusRollbackSupported = 0x0001,
    kStatusTrustConfigured = 0x0002,
    kStatusPeerCompatible = 0x0004,
    kStatusMaintenanceExclusive = 0x0008,
    kStatusHealthPassed = 0x0010,
    kStatusLocalBootValidated = 0x0020,
};
constexpr uint32_t kKnownStatusFlags =
    kStatusRollbackSupported | kStatusTrustConfigured |
    kStatusPeerCompatible | kStatusMaintenanceExclusive |
    kStatusHealthPassed | kStatusLocalBootValidated;

constexpr size_t kCommandResponsePayloadSize = 24;
struct CommandResponsePayload {
    State state = State::Idle;
    Command last_command = Command::Hello;
    NackReason reason = NackReason::None;
    uint32_t next_offset = 0;
    uint32_t target_generation = 0;
    uint32_t active_generation = 0;
    uint32_t status_flags = 0;
};

inline bool validCommand(Command command);

struct Frame {
    Target target = Target::Host;
    Command command = Command::Hello;
    uint8_t flags = 0;
    uint32_t transaction_id = 0;
    uint32_t offset = 0;
    uint32_t sequence = 0;
    uint16_t length = 0;
    uint8_t payload[kMaxChunkSize]{};
};

enum class FeedResult : uint8_t {
    None,
    FrameReady,
    FrameError,
};

inline uint16_t readLe16(const uint8_t *p) {
    return static_cast<uint16_t>(p[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8);
}

inline uint32_t readLe32(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

inline void writeLe16(uint8_t *p, uint16_t value) {
    p[0] = static_cast<uint8_t>(value);
    p[1] = static_cast<uint8_t>(value >> 8);
}

inline void writeLe32(uint8_t *p, uint32_t value) {
    p[0] = static_cast<uint8_t>(value);
    p[1] = static_cast<uint8_t>(value >> 8);
    p[2] = static_cast<uint8_t>(value >> 16);
    p[3] = static_cast<uint8_t>(value >> 24);
}

inline void encodeCommandResponsePayload(
    const CommandResponsePayload &status,
    uint8_t out[kCommandResponsePayloadSize]) {
    out[0] = kWireVersion;
    out[1] = static_cast<uint8_t>(status.state);
    out[2] = static_cast<uint8_t>(status.last_command);
    out[3] = 0;
    writeLe16(out + 4, static_cast<uint16_t>(status.reason));
    writeLe16(out + 6, 0);
    writeLe32(out + 8, status.next_offset);
    writeLe32(out + 12, status.target_generation);
    writeLe32(out + 16, status.active_generation);
    writeLe32(out + 20, status.status_flags);
}

inline bool decodeCommandResponsePayload(const uint8_t *wire, size_t length,
                                         CommandResponsePayload &status) {
    if (wire == nullptr || length != kCommandResponsePayloadSize ||
        wire[0] != kWireVersion || wire[3] != 0 || readLe16(wire + 6) != 0)
        return false;
    const auto state = static_cast<State>(wire[1]);
    const auto command = static_cast<Command>(wire[2]);
    if (static_cast<uint8_t>(state) >
            static_cast<uint8_t>(State::PendingConfirm) ||
        !validCommand(command) ||
        readLe16(wire + 4) >
            static_cast<uint16_t>(NackReason::RollbackUnavailable) ||
        (readLe32(wire + 20) & ~kKnownStatusFlags) != 0)
        return false;
    status.state = state;
    status.last_command = command;
    status.reason = static_cast<NackReason>(readLe16(wire + 4));
    status.next_offset = readLe32(wire + 8);
    status.target_generation = readLe32(wire + 12);
    status.active_generation = readLe32(wire + 16);
    status.status_flags = readLe32(wire + 20);
    return true;
}

inline uint32_t crc32(const uint8_t *data, size_t length) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; ++bit) {
            const uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

inline bool validTarget(Target target) {
    return target == Target::Host || target == Target::Left ||
           target == Target::Right;
}

inline bool validCommand(Command command) {
    switch (command) {
    case Command::Hello:
    case Command::EnterMaintenance:
    case Command::Manifest:
    case Command::Data:
    case Command::Prepare:
    case Command::Status:
    case Command::Activate:
    case Command::Confirm:
    case Command::Abort:
    case Command::ExitMaintenance:
    case Command::Ack:
    case Command::Nack:
    case Command::StatusResponse:
        return true;
    }
    return false;
}

inline bool validFrameFlags(Command command, uint8_t flags) {
    if ((flags & ~kKnownFrameFlags) != 0) return false;
    switch (command) {
    case Command::Data:
        return (flags & ~(kFlagFinal | kFlagRetransmit)) == 0;
    case Command::Status:
        return flags == kFlagStatusV2 ||
               flags == (kFlagStatusV2 | kFlagRetransmit);
    case Command::Ack:
    case Command::Nack:
        return flags == kFlagResponse;
    case Command::StatusResponse:
        return flags == (kFlagResponse | kFlagStatusV2);
    case Command::Hello:
    case Command::EnterMaintenance:
    case Command::Manifest:
    case Command::Prepare:
    case Command::Activate:
    case Command::Confirm:
    case Command::Abort:
    case Command::ExitMaintenance:
        return flags == 0 || flags == kFlagRetransmit;
    }
    return false;
}

inline size_t encodeFrame(const Frame &frame, uint8_t *wire, size_t capacity) {
    if (wire == nullptr || frame.length > kMaxChunkSize ||
        frame.transaction_id == 0 || frame.sequence == 0 ||
        frame.sequence == UINT32_MAX ||
        !validTarget(frame.target) ||
        !validCommand(frame.command) ||
        !validFrameFlags(frame.command, frame.flags)) return 0;
    const size_t total = kHeaderSize + frame.length + kCrcSize;
    if (capacity < total) return 0;
    memcpy(wire, kMagic, sizeof(kMagic));
    wire[4] = kWireVersion;
    wire[5] = static_cast<uint8_t>(frame.target);
    wire[6] = static_cast<uint8_t>(frame.command);
    wire[7] = frame.flags;
    writeLe32(wire + 8, frame.transaction_id);
    writeLe32(wire + 12, frame.offset);
    writeLe16(wire + 16, frame.length);
    writeLe16(wire + 18, 0);
    writeLe32(wire + 20, frame.sequence);
    if (frame.length != 0)
        memcpy(wire + kHeaderSize, frame.payload, frame.length);
    writeLe32(wire + kHeaderSize + frame.length,
              crc32(wire + 4, kHeaderSize - 4 + frame.length));
    return total;
}

inline bool decodeFrame(const uint8_t *wire, size_t length, Frame &frame) {
    if (wire == nullptr || length < kHeaderSize + kCrcSize ||
        memcmp(wire, kMagic, sizeof(kMagic)) != 0 ||
        wire[4] != kWireVersion || readLe16(wire + 18) != 0) return false;
    const uint16_t payload_length = readLe16(wire + 16);
    if (payload_length > kMaxChunkSize ||
        length != kHeaderSize + payload_length + kCrcSize) return false;
    const uint32_t expected = readLe32(wire + kHeaderSize + payload_length);
    if (crc32(wire + 4, kHeaderSize - 4 + payload_length) != expected)
        return false;
    // Avoid a kMaxChunkSize Frame temporary on small embedded task stacks.
    memset(&frame, 0, sizeof(frame));
    frame.target = static_cast<Target>(wire[5]);
    frame.command = static_cast<Command>(wire[6]);
    if (!validTarget(frame.target) || !validCommand(frame.command)) return false;
    frame.flags = wire[7];
    frame.transaction_id = readLe32(wire + 8);
    frame.offset = readLe32(wire + 12);
    frame.length = payload_length;
    frame.sequence = readLe32(wire + 20);
    if (frame.transaction_id == 0 || frame.sequence == 0 ||
        frame.sequence == UINT32_MAX ||
        !validFrameFlags(frame.command, frame.flags))
        return false;
    if (payload_length != 0)
        memcpy(frame.payload, wire + kHeaderSize, payload_length);
    return true;
}

// Decoder for the exclusive maintenance UART. It deliberately does not
// multiplex normal bridge traffic: callers must stop the real-time bridge
// before feeding this decoder.
class StreamDecoder final {
public:
    FeedResult feed(uint8_t byte, Frame &frame) {
        if (length_ < sizeof(kMagic)) {
            if (byte == kMagic[length_]) {
                buffer_[length_++] = byte;
            } else {
                length_ = byte == kMagic[0] ? 1u : 0u;
                if (length_ == 1) buffer_[0] = byte;
            }
            return FeedResult::None;
        }

        if (length_ >= sizeof(buffer_)) {
            reset();
            return FeedResult::FrameError;
        }
        buffer_[length_++] = byte;
        if (length_ == kHeaderSize) {
            if (buffer_[4] != kWireVersion || readLe16(buffer_ + 18) != 0) {
                reset();
                return FeedResult::FrameError;
            }
            const uint16_t payload_length = readLe16(buffer_ + 16);
            if (payload_length > kMaxChunkSize) {
                reset();
                return FeedResult::FrameError;
            }
            expected_ = kHeaderSize + payload_length + kCrcSize;
        }
        if (expected_ != 0 && length_ == expected_) {
            const bool ok = decodeFrame(buffer_, length_, frame);
            reset();
            return ok ? FeedResult::FrameReady : FeedResult::FrameError;
        }
        return FeedResult::None;
    }

    void reset() {
        length_ = 0;
        expected_ = 0;
    }

    size_t buffered() const { return length_; }

private:
    uint8_t buffer_[kMaxWireSize]{};
    size_t length_ = 0;
    size_t expected_ = 0;
};

} // namespace makcm::ota
