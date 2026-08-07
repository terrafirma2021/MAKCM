#include "ControllerCodec.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>

using namespace makcm::stick;

namespace {

bool near(float left, float right, float epsilon = 0.0002f) {
    return std::fabs(left - right) <= epsilon;
}

ControllerProfileIdentity xboxIdentity() {
    ControllerProfileIdentity identity;
    identity.vid = 0x4131;
    identity.pid = 0x3519;
    identity.bcdDevice = 0x0100;
    identity.descriptorHash = 0xAABBCCDD;
    identity.hidReportHash = 0x11223344;
    identity.endpointAddress = 0x81;
    identity.reportLength = 8;
    identity.formatVersion = 1;
    return identity;
}

} // namespace

int main() {
    auto identity = xboxIdentity();
    ControllerProfile xbox = makeXboxInt16LeProfile(
        identity, int16LeAxis(0), int16LeAxis(2, 100, -30000, 30000, -1));
    ControllerReportCodec codec;
    assert(codec.configure(xbox));

    uint8_t report[8] = {0, 0, 100, 0, 0xA5, 0x5A, 0xCC, 0x33};
    StickVector physical;
    assert(codec.decodePhysical(report, sizeof(report), physical));
    assert(near(physical.x, 0.0f) && near(physical.y, 0.0f));

    // Asymmetric int16 range and sign inversion: raw +30000 on Y is internal
    // -1 because the profile declares a negative Y sign.
    report[2] = static_cast<uint8_t>(30000 & 0xFF);
    report[3] = static_cast<uint8_t>(30000 >> 8);
    assert(codec.decodePhysical(report, sizeof(report), physical));
    assert(near(physical.y, -1.0f));
    report[2] = static_cast<uint8_t>(-30000 & 0xFF);
    report[3] = static_cast<uint8_t>(static_cast<uint16_t>(-30000) >> 8);
    assert(codec.decodePhysical(report, sizeof(report), physical));
    assert(near(physical.y, 1.0f));

    // Compose copies all bytes and mutates only explicitly allowed axis bytes.
    const uint8_t original[8] = {0x10, 0x20, 0x64, 0x00, 0xA5, 0x5A, 0xCC, 0x33};
    uint8_t compose[8]{};
    MutationResult mutation;
    assert(codec.composeCopy(original, compose, sizeof(compose), {0.35f, 0.35f},
                             &mutation));
    assert(mutation.copied && mutation.changed && mutation.changedBytes == 4);
    assert(compose[4] == original[4] && compose[5] == original[5] &&
           compose[6] == original[6] && compose[7] == original[7]);
    // +0.35 on an int16 positive side is rounded using max 32767.
    const int16_t expectedX = static_cast<int16_t>(std::lround(0.35 * 32767.0));
    const int16_t expectedYRaw = static_cast<int16_t>(100 -
        std::lround(0.35 * (100 - (-30000))));
    const int16_t encodedX = static_cast<int16_t>(static_cast<uint16_t>(compose[0]) |
                                                  (static_cast<uint16_t>(compose[1]) << 8));
    const int16_t encodedY = static_cast<int16_t>(static_cast<uint16_t>(compose[2]) |
                                                  (static_cast<uint16_t>(compose[3]) << 8));
    assert(encodedX == expectedX);
    assert(encodedY == expectedYRaw);

    // Wrong report ID and wrong length fail before touching a compose copy.
    ControllerProfileIdentity dualIdentity;
    dualIdentity.vid = 0x054C;
    dualIdentity.pid = 0x0DF2;
    dualIdentity.bcdDevice = 0x0200;
    dualIdentity.descriptorHash = 0xCAFEBABE;
    dualIdentity.hidReportHash = 0x01020304;
    dualIdentity.endpointAddress = 0x83;
    dualIdentity.reportId = 1;
    dualIdentity.reportLength = 8;
    ControllerProfile dualProfile = makeDualSenseUsbUint8Profile(
        dualIdentity, uint8Axis(3), uint8Axis(4, 128, 0, 255, -1));
    DualSenseUsbUint8Codec dualCodec;
    assert(dualCodec.configureExact(dualProfile, dualIdentity));
    uint8_t dualReport[8] = {1, 0xAB, 0xCD, 128, 128, 0xEE, 0x12, 0x34};
    assert(dualCodec.decodePhysical(dualReport, sizeof(dualReport), physical));
    assert(near(physical.x, 0.0f) && near(physical.y, 0.0f));
    dualReport[3] = 255;
    dualReport[4] = 0;
    assert(dualCodec.decodePhysical(dualReport, sizeof(dualReport), physical));
    assert(near(physical.x, 1.0f) && near(physical.y, 1.0f));
    dualReport[0] = 2;
    CodecError error = CodecError::None;
    assert(!dualCodec.decodePhysical(dualReport, sizeof(dualReport), physical, &error));
    assert(error == CodecError::WrongReportId);
    dualReport[0] = 1;
    assert(!dualCodec.decodePhysical(dualReport, sizeof(dualReport) - 1, physical, &error));
    assert(error == CodecError::WrongLength);

    // Identity mismatch refuses arming; changing VID/PID/hash/variant is not
    // a wildcard match even when the report bytes happen to look compatible.
    ControllerProfileIdentity wrongIdentity = dualIdentity;
    wrongIdentity.bcdDevice++;
    DualSenseUsbUint8Codec unknown;
    assert(!unknown.configureExact(dualProfile, wrongIdentity));
    uint8_t untouched[8];
    std::memcpy(untouched, dualReport, sizeof(untouched));
    MutationResult rejected;
    assert(!unknown.composeCopy(dualReport, untouched, sizeof(untouched),
                                {0.5f, -0.5f}, &rejected));
    assert(rejected.error == CodecError::ProfileNotConfigured ||
           rejected.error == CodecError::ProfileMismatch);
    assert(std::memcmp(untouched, dualReport, sizeof(untouched)) == 0);

    // The mutation guard catches any byte outside the two configured fields.
    uint8_t tampered[8];
    std::memcpy(tampered, dualReport, sizeof(tampered));
    tampered[7] ^= 0xFF;
    MutationResult guard;
    assert(!dualCodec.validateUntouched(dualReport, tampered, sizeof(tampered), &guard));
    return 0;
}

