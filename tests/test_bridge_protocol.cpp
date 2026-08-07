#include "BridgeProtocol.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

using makcm::bridge::FeedResult;
using makcm::bridge::Frame;
using makcm::bridge::MessageType;
using makcm::bridge::MixedStreamDecoder;

int main() {
    uint8_t payload[64];
    for (size_t i = 0; i < sizeof(payload); ++i) {
        payload[i] = static_cast<uint8_t>((i % 7 == 0) ? 0 : i);
    }

    uint8_t wire[makcm::bridge::kMaxWireFrameSize];
    const size_t wireLength = makcm::bridge::encodeFrame(
        MessageType::DseInput, 0x1234, payload, sizeof(payload), wire, sizeof(wire),
        0x89ABCDEF);
    assert(wireLength != 0);
    assert(wire[0] == 0);
    assert(wire[wireLength - 1] == 0);

    MixedStreamDecoder decoder;
    Frame decoded;
    uint8_t textByte = 0;
    FeedResult result = FeedResult::None;
    for (size_t i = 0; i < wireLength; ++i) {
        result = decoder.feed(wire[i], decoded, textByte);
    }
    assert(result == FeedResult::FrameReady);
    assert(decoded.type == MessageType::DseInput);
    assert(decoded.sequence == 0x1234);
    assert(decoded.session == 0x89ABCDEF);
    assert(decoded.length == sizeof(payload));
    assert(memcmp(decoded.payload, payload, sizeof(payload)) == 0);

    // A lost leading delimiter must be recovered at the trailing delimiter
    // instead of leaving the mixed decoder one frame out of phase forever.
    MixedStreamDecoder missingLeadingDecoder;
    Frame missingLeadingFrame;
    bool recoveredMissingLeading = false;
    for (size_t i = 1; i < wireLength; ++i) {
        if (missingLeadingDecoder.feed(wire[i], missingLeadingFrame,
                                       textByte) == FeedResult::FrameReady) {
            recoveredMissingLeading = true;
        }
    }
    assert(recoveredMissingLeading);
    assert(missingLeadingFrame.sequence == 0x1234);
    assert(missingLeadingFrame.session == 0x89ABCDEF);
    assert(missingLeadingFrame.length == sizeof(payload));
    assert(memcmp(missingLeadingFrame.payload, payload, sizeof(payload)) == 0);

    // Consecutive frames share two delimiters.  Losing either one at the
    // boundary must still deliver both frames.
    MixedStreamDecoder boundaryDecoder;
    Frame boundaryFrame;
    size_t boundaryFrames = 0;
    for (size_t i = 0; i < wireLength; ++i) {
        if (boundaryDecoder.feed(wire[i], boundaryFrame, textByte) ==
            FeedResult::FrameReady) {
            ++boundaryFrames;
        }
    }
    for (size_t i = 1; i < wireLength; ++i) {
        if (boundaryDecoder.feed(wire[i], boundaryFrame, textByte) ==
            FeedResult::FrameReady) {
            ++boundaryFrames;
        }
    }
    assert(boundaryFrames == 2);
    assert(boundaryFrame.sequence == 0x1234);

    makcm::bridge::DeviceProfile profile;
    profile.kind = makcm::bridge::DeviceKind::RawUsb;
    profile.interfaceClass = 3;
    profile.inPacketSize = 64;
    profile.reportDescriptorLength = 273;
    profile.deviceDescriptorLength = 18;
    profile.configurationDescriptorLength = 96;
    profile.endpointCount = 4;
    const size_t profileWireLength = makcm::bridge::encodeFrame(
        MessageType::ProfileBegin, 7,
        reinterpret_cast<const uint8_t *>(&profile), sizeof(profile),
        wire, sizeof(wire));
    assert(profileWireLength != 0);

    uint8_t maximumPayload[makcm::bridge::kMaxPayloadSize];
    memset(maximumPayload, 0xA5, sizeof(maximumPayload));
    const size_t maximumWireLength = makcm::bridge::encodeFrame(
        MessageType::ProfileDescriptorChunk, 8, maximumPayload,
        sizeof(maximumPayload), wire, sizeof(wire));
    assert(maximumWireLength != 0);

    makcm::bridge::ControlRequestHeader control;
    control.requestId = 0xCAFE;
    control.bmRequestType = 0xA1;
    control.bRequest = 0x01;
    control.wValue = 0x0305;
    control.wIndex = 2;
    control.wLength = 64;
    const size_t controlWireLength = makcm::bridge::encodeFrame(
        MessageType::ControlRequest, 9,
        reinterpret_cast<const uint8_t *>(&control), sizeof(control),
        wire, sizeof(wire), 0x10203040);
    assert(controlWireLength != 0);

    makcm::bridge::ControlOutDataHeader out;
    out.request.requestId = 0x1234;
    out.request.bmRequestType = 0x21;
    out.request.bRequest = 0x09;
    out.request.wValue = 1;
    out.request.wIndex = 2;
    out.request.wLength = 3;
    out.request.dataLength = 3;
    out.offset = 0;
    out.totalLength = 3;
    const uint8_t outBytes[] = {0xAA, 0x55, 0x00};
    uint8_t outPayload[sizeof(out) + sizeof(outBytes)] = {};
    memcpy(outPayload, &out, sizeof(out));
    memcpy(outPayload + sizeof(out), outBytes, sizeof(outBytes));
    const size_t outWireLength = makcm::bridge::encodeFrame(
        MessageType::ControlOutData, 10, outPayload, sizeof(outPayload),
        wire, sizeof(wire), 0x10203040);
    assert(outWireLength != 0);

    const size_t streamReadyWire = makcm::bridge::encodeFrame(
        MessageType::StreamReady, 11, nullptr, 0, wire, sizeof(wire),
        0x10203040);
    assert(streamReadyWire != 0);

    const char text[] = "USB_HELLO\n";
    for (size_t i = 0; i < sizeof(text) - 1; ++i) {
        result = decoder.feed(static_cast<uint8_t>(text[i]), decoded, textByte);
        assert(result == FeedResult::TextByte);
        assert(textByte == static_cast<uint8_t>(text[i]));
    }

    const size_t corruptWireLength = makcm::bridge::encodeFrame(
        MessageType::HidInput, 0x1234, payload, sizeof(payload),
        wire, sizeof(wire), 0x89ABCDEF);
    assert(corruptWireLength == wireLength);
    wire[corruptWireLength / 2] ^= 0x40;
    for (size_t i = 0; i < corruptWireLength; ++i) {
        result = decoder.feed(wire[i], decoded, textByte);
    }
    assert(result == FeedResult::FrameError);
    return 0;
}
