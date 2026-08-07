#include "ProxyCapabilityPlanner.h"
#include "proxy_policy.h"

#include <cassert>

using makcm::bridge::CapabilityInput;
using makcm::bridge::DeviceKind;
using makcm::bridge::DeviceProfile;
using makcm::bridge::ProfileBlobType;
using makcm::bridge::UsbSpeed;
using makcm::native::ProfileStore;
using makcm::native::ProxyPolicy;

namespace {

// Deliberately use a VID/PID and endpoint numbers that are not the target
// 4131:3519 / EP83 tuple.  The mouse endpoint must still be selected from the
// HID report grammar, not from a device-specific switch or an endpoint
// number inherited from the Xbox fixture.
const uint8_t kDevice[] = {
    18, 1, 0x00, 0x02, 0, 0, 0, 64, 0x34, 0x12, 0x78, 0x56,
    0, 1, 1, 2, 3, 1,
};

const uint8_t kMouseReport[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x05, 0x09, 0x19, 0x01,
    0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01,
    0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x03, 0x05, 0x01,
    0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08,
    0x95, 0x02, 0x81, 0x06, 0xC0,
};

const uint8_t kConfiguration[] = {
    9, 2, 57, 0, 2, 1, 0, 0x80, 50,
    9, 4, 0, 0, 2, 0xFF, 0, 0, 0,
    7, 5, 0x84, 3, 16, 0, 1,
    7, 5, 0x04, 3, 16, 0, 1,
    9, 4, 1, 0, 1, 3, 0, 0, 0,
    9, 0x21, 0x11, 0x01, 0, 1, 0x22,
    static_cast<uint8_t>(sizeof(kMouseReport)), 0,
    7, 5, 0x86, 3, 8, 0, 1,
};

bool append(ProfileStore &store, ProfileBlobType type, const uint8_t *data,
            uint16_t length) {
    makcm::bridge::ProfileBlobChunkHeader header;
    header.type = type;
    header.totalLength = length;
    header.totalCrc = makcm::bridge::crc16Ccitt(data, length);
    header.offset = 0;
    return store.appendProfileBlob(header, data, length);
}

} // namespace

int main() {
    DeviceProfile profile;
    profile.kind = DeviceKind::RawUsb;
    profile.vid = 0x1234;
    profile.pid = 0x5678;
    profile.usbSpeed = UsbSpeed::Full;
    profile.deviceDescriptorLength = sizeof(kDevice);
    profile.configurationDescriptorLength = sizeof(kConfiguration);
    profile.reportDescriptorLength = sizeof(kMouseReport);
    profile.endpointCount = 3;
    profile.inEndpoint = 0x84;
    profile.configAttributes = 0x80;

    ProfileStore store;
    assert(store.begin(profile));
    assert(append(store, ProfileBlobType::Device, kDevice, sizeof(kDevice)));
    assert(append(store, ProfileBlobType::Configuration, kConfiguration,
                  sizeof(kConfiguration)));
    assert(store.appendHidReportDescriptor(
        1, 0, kMouseReport, sizeof(kMouseReport)));

    CapabilityInput input{kDevice, sizeof(kDevice), kConfiguration,
                          sizeof(kConfiguration), UsbSpeed::Full, 4000000};
    store.setCapabilityResult(makcm::bridge::planCapability(input));
    assert(store.validate().ok);

    const auto *mouse = store.mouseEndpoint();
    assert(mouse != nullptr && mouse->address == 0x86);
    ProxyPolicy policy;
    policy.configure(&store);
    assert(policy.isMouseEndpoint(0x86));
    assert(!policy.isMouseEndpoint(0x83));
    return 0;
}
