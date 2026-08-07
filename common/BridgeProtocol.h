#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace makcm {
namespace bridge {

constexpr uint8_t kProtocolVersion = 5;
// 64-byte USB reports need one metadata byte when a HID report ID is
// transported separately. Keep some headroom for profile/control packets.
constexpr size_t kMaxPayloadSize = 192;
// Every binary frame carries a physical-connection session ID. This prevents
// reports left in the UART/FreeRTOS queues by a removed device from being
// delivered to the next device after a fast hot-plug.
constexpr size_t kRawHeaderSize = 10;
constexpr size_t kCrcSize = 2;
constexpr size_t kMaxRawFrameSize = kRawHeaderSize + kMaxPayloadSize + kCrcSize;
constexpr size_t kMaxEncodedFrameSize = kMaxRawFrameSize + (kMaxRawFrameSize / 254) + 1;
constexpr size_t kMaxWireFrameSize = kMaxEncodedFrameSize + 2;

enum class MessageType : uint8_t {
    // Generic endpoint traffic. The first payload byte is the original USB
    // endpoint address; the remaining bytes are the unmodified transfer.
    EndpointIn = 0x01,
    EndpointOut = 0x02,
    HidFeatureSet = 0x03,
    HidFeatureGet = 0x04,
    HidFeatureResponse = 0x05,
    VendorInput = 0x06,
    VendorOutput = 0x07,
    MouseInput = 0x08,
    ControlRequest = 0x09,
    ControlResponse = 0x0A,
    DeviceStatus = 0x10,
    ProfileBegin = 0x11,
    ProfileDescriptorChunk = 0x12,
    ProfileReady = 0x13,
    DeviceGone = 0x14,
    ProfileAccepted = 0x15,
    DeviceGoneAccepted = 0x16,
    StreamStarted = 0x17,
    ProfileDeviceDescriptorChunk = 0x18,
    ProfileConfigurationChunk = 0x19,
    // Raw UTF-16 USB string descriptor. Payload metadata is index (u8),
    // LANGID (u16), byte offset (u16), followed by descriptor bytes.
    ProfileStringDescriptorChunk = 0x1A,
    // A cached, deterministic control-IN response. Payload starts with
    // ProfileControlCacheHeader and is followed by a chunk of the response.
    // This is used for enumeration-time vendor descriptors (for example the
    // Microsoft OS 1.0 Extended Compat ID) so EP0 does not depend on a live
    // UART round-trip while Windows is binding a driver.
    ProfileControlCacheChunk = 0x1B,

    // Descriptor-agnostic profile transport. The legacy descriptor messages
    // above remain accepted during a rolling update, while new firmware uses
    // one blob header for every descriptor kind.
    ProfileBlobChunk = 0x1C,
    CapabilityResult = 0x1D,
    ControlSetup = 0x1E,
    ControlInResult = 0x1F,
    ControlOutData = 0x20,
    EndpointInData = 0x21,
    EndpointOutData = 0x22,
    StallHalt = 0x23,
    BusReset = 0x24,
    SetConfiguration = 0x25,
    SetInterface = 0x26,
    SuspendResume = 0x27,
    // The right host does not open its endpoint-input gate until the left
    // device stack has consumed StreamStarted and confirmed that all profiled
    // endpoints are live. This closes the lost-StreamStarted window.
    StreamReady = 0x28,

    // Source compatibility while the two firmware halves migrate to the
    // descriptor-driven endpoint transport.
    HidInput = EndpointIn,
    HidOutput = EndpointOut,
    DseInput = EndpointIn,
    DseOutput = EndpointOut,
    DseFeatureSet = HidFeatureSet,
    DseFeatureSnapshot = HidFeatureResponse,
};

enum class DeviceKind : uint8_t {
    None = 0,
    Hid = 1,
    XboxVendor = 2,
    RawUsb = 3,
};

enum class UsbSpeed : uint8_t {
    Unknown = 0,
    Low = 1,
    Full = 2,
    High = 3,
};

struct __attribute__((packed)) DeviceProfile {
    uint8_t version = kProtocolVersion;
    DeviceKind kind = DeviceKind::None;
    uint8_t interfaceNumber = 0;
    uint8_t interfaceClass = 0;
    uint8_t interfaceSubClass = 0;
    uint8_t interfaceProtocol = 0;
    uint8_t inEndpoint = 0;
    uint8_t outEndpoint = 0;
    uint16_t inPacketSize = 0;
    uint16_t outPacketSize = 0;
    uint8_t inInterval = 0;
    uint8_t outInterval = 0;
    // Optional second HID mouse interface. It is currently used by composite
    // XInput devices whose primary interface is vendor-specific.
    uint8_t auxiliaryInterfaceNumber = 0xFF;
    uint8_t auxiliaryInterfaceClass = 0;
    uint8_t auxiliaryInterfaceSubClass = 0;
    uint8_t auxiliaryInterfaceProtocol = 0;
    uint8_t auxiliaryInEndpoint = 0;
    uint8_t auxiliaryOutEndpoint = 0;
    uint16_t auxiliaryInPacketSize = 0;
    uint16_t auxiliaryOutPacketSize = 0;
    uint8_t auxiliaryInInterval = 0;
    uint8_t auxiliaryOutInterval = 0;
    uint16_t reportDescriptorLength = 0;
    uint8_t flags = 0;
    uint16_t vid = 0;
    uint16_t pid = 0;
    uint16_t bcdUsb = 0x0200;
    uint16_t bcdDevice = 0x0100;
    uint8_t deviceClass = 0;
    uint8_t deviceSubClass = 0;
    uint8_t deviceProtocol = 0;
    uint8_t configAttributes = 0x80;
    uint16_t configPowerMa = 500;
    char manufacturer[40] = {};
    char product[48] = {};
    char serial[40] = {};
    // Raw descriptors are authoritative in protocol v5. The legacy fields
    // above remain as convenient observer metadata and for rolling upgrades.
    uint16_t deviceDescriptorLength = 0;
    uint16_t configurationDescriptorLength = 0;
    uint8_t endpointCount = 0;
    // Profile planning metadata.  These fields are advisory until the
    // CapabilityResult frame is received, but they let the left side reject
    // an incomplete/unsupported profile before asserting D+.
    UsbSpeed usbSpeed = UsbSpeed::Unknown;
    uint8_t configurationCount = 0;
    uint8_t blobCount = 0;
    uint8_t profileFlags = 0;
    uint16_t bosDescriptorLength = 0;
    uint16_t qualifierDescriptorLength = 0;
    uint32_t descriptorCrc = 0;
};

constexpr uint8_t kProfileFlagReportIds = 0x01;
constexpr size_t kMaxReportDescriptorSize = 512;
constexpr size_t kMaxConfigurationDescriptorSize = 1024;
constexpr size_t kMaxHidInterfaces = 8;
// Keep every referenced UTF-16 string for each advertised LANGID.  A
// composite device can legally expose several language tables; twelve slots
// only covered one language plus the optional MS OS string.
constexpr size_t kMaxStringDescriptors = 32;
constexpr size_t kMaxStringDescriptorSize = 254;
constexpr size_t kMaxCachedControls = 4;
constexpr size_t kMaxCachedControlSize = 192;
constexpr size_t kMaxBosDescriptorSize = 256;
constexpr size_t kMaxQualifierDescriptorSize = 10;
constexpr size_t kMaxProfileBlobSize = 1024;
constexpr size_t kMaxProfileBlobs = 16;

enum class ProxyCapability : uint8_t {
    FullProxy = 0,
    ProxyWithEndpointRemap = 1,
    LimitedProxy = 2,
    Unsupported = 3,
};

enum class ProfileBlobType : uint8_t {
    Device = 1,
    Configuration = 2,
    String = 3,
    Bos = 4,
    DeviceQualifier = 5,
    HidReport = 6,
    Unknown = 7,
};

// Every ProfileBlobChunk carries its complete identity and integrity fields.
// `key` is an interface number for HidReport, a string index for String, and
// zero for singleton blobs. String blobs additionally carry LANGID.
struct __attribute__((packed)) ProfileBlobChunkHeader {
    ProfileBlobType type = ProfileBlobType::Unknown;
    uint8_t flags = 0;
    uint16_t key = 0;
    uint16_t langid = 0;
    uint16_t offset = 0;
    uint16_t totalLength = 0;
    uint16_t totalCrc = 0;
};

enum class CapabilityReason : uint8_t {
    None = 0,
    HighSpeedOnly = 1,
    HubDevice = 2,
    MultipleConfigurations = 3,
    TooManyEndpoints = 4,
    TooManyInEndpoints = 5,
    TooManyOutEndpoints = 6,
    EndpointNumberOutOfRange = 7,
    IsochronousUnsupported = 8,
    AlternateSettingUnsupported = 9,
    PacketSizeUnsupported = 10,
    Ep0PacketSizeUnsupported = 11,
    DescriptorMalformed = 12,
    UartBudgetExceeded = 13,
    BlobIncomplete = 14,
    BlobCrcMismatch = 15,
};

struct __attribute__((packed)) CapabilityResult {
    ProxyCapability capability = ProxyCapability::Unsupported;
    CapabilityReason reason = CapabilityReason::None;
    UsbSpeed speed = UsbSpeed::Unknown;
    uint8_t configurationCount = 0;
    uint8_t interfaceCount = 0;
    uint8_t endpointCount = 0;
    uint8_t inCount = 0;
    uint8_t outCount = 0;
    uint8_t maxEndpointNumber = 0;
    uint16_t maxPacketSize = 0;
    uint32_t estimatedBitsPerSecond = 0;
};

struct __attribute__((packed)) ProfileControlCacheHeader {
    uint8_t bmRequestType = 0;
    uint8_t bRequest = 0;
    uint16_t wValue = 0;
    uint16_t wIndex = 0;
    uint16_t wLength = 0;
    uint16_t offset = 0;
    uint16_t totalLength = 0;
};

struct __attribute__((packed)) ControlRequestHeader {
    uint16_t requestId = 0;
    uint8_t bmRequestType = 0;
    uint8_t bRequest = 0;
    uint16_t wValue = 0;
    uint16_t wIndex = 0;
    uint16_t wLength = 0;
    uint16_t dataLength = 0;
};

enum class ControlStatus : uint8_t {
    Completed = 0,
    Stalled = 1,
    Error = 2,
};

struct __attribute__((packed)) ControlResponseHeader {
    uint16_t requestId = 0;
    ControlStatus status = ControlStatus::Error;
    uint8_t flags = 0;
    uint16_t dataLength = 0;
};

enum ControlResponseFlags : uint8_t {
    kControlFlagShortPacket = 0x01,
    kControlFlagZlpRequired = 0x02,
};

// Generic endpoint payload prefix used by EndpointInData/EndpointOutData.
// Keeping this as a named type prevents one side from silently changing the
// endpoint/flags ordering while legacy frames are still accepted.
struct __attribute__((packed)) EndpointDataHeader {
    uint8_t endpoint = 0;
    uint8_t flags = 0;
};

enum EndpointTransferFlags : uint8_t {
    kEndpointFlagStall = 0x01,
    kEndpointFlagReset = 0x02,
    kEndpointFlagShortPacket = 0x04,
    kEndpointFlagTransferError = 0x08,
};

struct __attribute__((packed)) EndpointStatusHeader {
    uint8_t endpoint = 0;
    uint8_t flags = 0;
};

struct __attribute__((packed)) SuspendResumeHeader {
    uint8_t suspended = 0;
    uint8_t remoteWakeup = 0;
};

struct __attribute__((packed)) SetInterfaceHeader {
    uint8_t interfaceNumber = 0;
    uint8_t alternateSetting = 0;
};

struct __attribute__((packed)) SetConfigurationHeader {
    uint8_t configuration = 0;
};

// OUT control data is self-contained so a failed UART enqueue cannot leave a
// half-created pending setup on the right host.  The repeated setup fields
// preserve the complete request while offset/totalLength identify a future
// split data stage.
struct __attribute__((packed)) ControlOutDataHeader {
    ControlRequestHeader request{};
    uint16_t offset = 0;
    uint16_t totalLength = 0;
};

struct Frame {
    MessageType type = MessageType::DeviceStatus;
    uint16_t sequence = 0;
    uint32_t session = 0;
    uint16_t length = 0;
    uint8_t payload[kMaxPayloadSize] = {};
};

enum class FeedResult : uint8_t {
    None,
    TextByte,
    FrameReady,
    FrameError,
};

inline uint16_t crc16Ccitt(const uint8_t *data, size_t length) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < length; ++i) {
        crc ^= static_cast<uint16_t>(data[i]) << 8;
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                                 : static_cast<uint16_t>(crc << 1);
        }
    }
    return crc;
}

inline size_t cobsEncode(const uint8_t *input, size_t length, uint8_t *output, size_t capacity) {
    if (capacity == 0) {
        return 0;
    }

    size_t readIndex = 0;
    size_t writeIndex = 1;
    size_t codeIndex = 0;
    uint8_t code = 1;

    while (readIndex < length) {
        if (input[readIndex] == 0) {
            if (codeIndex >= capacity) {
                return 0;
            }
            output[codeIndex] = code;
            code = 1;
            codeIndex = writeIndex++;
            if (writeIndex > capacity) {
                return 0;
            }
            ++readIndex;
        } else {
            if (writeIndex >= capacity) {
                return 0;
            }
            output[writeIndex++] = input[readIndex++];
            if (++code == 0xFF) {
                if (codeIndex >= capacity) {
                    return 0;
                }
                output[codeIndex] = code;
                code = 1;
                codeIndex = writeIndex++;
                if (writeIndex > capacity) {
                    return 0;
                }
            }
        }
    }

    if (codeIndex >= capacity) {
        return 0;
    }
    output[codeIndex] = code;
    return writeIndex;
}

inline size_t cobsDecode(const uint8_t *input, size_t length, uint8_t *output, size_t capacity) {
    size_t readIndex = 0;
    size_t writeIndex = 0;

    while (readIndex < length) {
        const uint8_t code = input[readIndex++];
        if (code == 0) {
            return 0;
        }

        const size_t copyLength = static_cast<size_t>(code - 1);
        if (readIndex + copyLength > length || writeIndex + copyLength > capacity) {
            return 0;
        }

        if (copyLength != 0) {
            memcpy(output + writeIndex, input + readIndex, copyLength);
            readIndex += copyLength;
            writeIndex += copyLength;
        }

        if (code != 0xFF && readIndex < length) {
            if (writeIndex >= capacity) {
                return 0;
            }
            output[writeIndex++] = 0;
        }
    }

    return writeIndex;
}

inline size_t encodeFrame(MessageType type,
                          uint16_t sequence,
                          const uint8_t *payload,
                          uint16_t payloadLength,
                          uint8_t *wire,
                          size_t wireCapacity,
                          uint32_t session = 0) {
    if (payloadLength > kMaxPayloadSize || wireCapacity < 3 ||
        (payloadLength != 0 && payload == nullptr)) {
        return 0;
    }

    uint8_t raw[kMaxRawFrameSize];
    raw[0] = kProtocolVersion;
    raw[1] = static_cast<uint8_t>(type);
    raw[2] = static_cast<uint8_t>(sequence & 0xFF);
    raw[3] = static_cast<uint8_t>(sequence >> 8);
    raw[4] = static_cast<uint8_t>(session);
    raw[5] = static_cast<uint8_t>(session >> 8);
    raw[6] = static_cast<uint8_t>(session >> 16);
    raw[7] = static_cast<uint8_t>(session >> 24);
    raw[8] = static_cast<uint8_t>(payloadLength & 0xFF);
    raw[9] = static_cast<uint8_t>(payloadLength >> 8);
    if (payloadLength != 0) {
        memcpy(raw + kRawHeaderSize, payload, payloadLength);
    }

    const size_t withoutCrc = kRawHeaderSize + payloadLength;
    const uint16_t crc = crc16Ccitt(raw, withoutCrc);
    raw[withoutCrc] = static_cast<uint8_t>(crc & 0xFF);
    raw[withoutCrc + 1] = static_cast<uint8_t>(crc >> 8);

    wire[0] = 0;
    const size_t encodedLength = cobsEncode(raw, withoutCrc + kCrcSize,
                                            wire + 1, wireCapacity - 2);
    if (encodedLength == 0) {
        return 0;
    }
    wire[encodedLength + 1] = 0;
    return encodedLength + 2;
}

inline bool decodeFrame(const uint8_t *encoded, size_t encodedLength, Frame &frame) {
    if (encoded == nullptr || encodedLength == 0 || encodedLength > kMaxEncodedFrameSize) {
        return false;
    }

    uint8_t raw[kMaxRawFrameSize];
    const size_t rawLength = cobsDecode(encoded, encodedLength, raw, sizeof(raw));
    if (rawLength < kRawHeaderSize + kCrcSize || raw[0] != kProtocolVersion) {
        return false;
    }

    const uint16_t payloadLength = static_cast<uint16_t>(raw[8]) |
                                   (static_cast<uint16_t>(raw[9]) << 8);
    if (payloadLength > kMaxPayloadSize ||
        rawLength != kRawHeaderSize + payloadLength + kCrcSize) {
        return false;
    }

    const size_t withoutCrc = rawLength - kCrcSize;
    const uint16_t expectedCrc = static_cast<uint16_t>(raw[withoutCrc]) |
                                 (static_cast<uint16_t>(raw[withoutCrc + 1]) << 8);
    if (crc16Ccitt(raw, withoutCrc) != expectedCrc) {
        return false;
    }

    frame.type = static_cast<MessageType>(raw[1]);
    frame.sequence = static_cast<uint16_t>(raw[2]) |
                     (static_cast<uint16_t>(raw[3]) << 8);
    frame.session = static_cast<uint32_t>(raw[4]) |
                    (static_cast<uint32_t>(raw[5]) << 8) |
                    (static_cast<uint32_t>(raw[6]) << 16) |
                    (static_cast<uint32_t>(raw[7]) << 24);
    frame.length = payloadLength;
    if (payloadLength != 0) {
        memcpy(frame.payload, raw + kRawHeaderSize, payloadLength);
    }
    return true;
}

class MixedStreamDecoder {
public:
    FeedResult feed(uint8_t byte, Frame &frame, uint8_t &textByte) {
        if (!binaryMode_) {
            if (byte == 0) {
                // Frames normally have both leading and trailing zero
                // delimiters.  If UART pressure drops exactly one delimiter
                // between frames, the old state machine emits the whole next
                // COBS payload as text and stays permanently out of phase.
                // Keep a bounded shadow of emitted text so the trailing zero
                // can validate and recover that frame.  Ordinary text is
                // still emitted byte-for-byte and is rejected here by the
                // complete header/length/CRC validation.
                const bool recovered =
                    !textCandidateOverflow_ && textCandidateLength_ != 0 &&
                    decodeFrame(textCandidate_, textCandidateLength_, frame);
                textCandidateLength_ = 0;
                textCandidateOverflow_ = false;
                if (recovered) {
                    // Stay in text mode; the next leading delimiter enters
                    // binary mode normally.
                    return FeedResult::FrameReady;
                }
                binaryMode_ = true;
                encodedLength_ = 0;
                overflow_ = false;
                return FeedResult::None;
            }
            if (textCandidateLength_ < sizeof(textCandidate_)) {
                textCandidate_[textCandidateLength_++] = byte;
            } else {
                textCandidateOverflow_ = true;
            }
            textByte = byte;
            return FeedResult::TextByte;
        }

        if (byte == 0) {
            binaryMode_ = false;
            if (overflow_ || encodedLength_ == 0) {
                return overflow_ ? FeedResult::FrameError : FeedResult::None;
            }
            return decodeFrame(encoded_, encodedLength_, frame)
                       ? FeedResult::FrameReady
                       : FeedResult::FrameError;
        }

        if (encodedLength_ >= sizeof(encoded_)) {
            overflow_ = true;
            return FeedResult::None;
        }
        encoded_[encodedLength_++] = byte;
        return FeedResult::None;
    }

    void reset() {
        binaryMode_ = false;
        encodedLength_ = 0;
        overflow_ = false;
        textCandidateLength_ = 0;
        textCandidateOverflow_ = false;
    }

private:
    bool binaryMode_ = false;
    bool overflow_ = false;
    size_t encodedLength_ = 0;
    uint8_t encoded_[kMaxEncodedFrameSize] = {};
    size_t textCandidateLength_ = 0;
    bool textCandidateOverflow_ = false;
    uint8_t textCandidate_[kMaxEncodedFrameSize] = {};
};

} // namespace bridge
} // namespace makcm
