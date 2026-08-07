#include "ProxyCapabilityPlanner.h"
#include "profile_store.h"

#include <cassert>

using makcm::bridge::CapabilityInput;
using makcm::bridge::DeviceKind;
using makcm::bridge::DeviceProfile;
using makcm::bridge::ProfileBlobChunkHeader;
using makcm::bridge::ProfileBlobType;
using makcm::bridge::UsbSpeed;
using makcm::native::ProfileStore;

namespace {
const uint8_t device[] = {
    18, 1, 0x00, 0x02, 0, 0, 0, 64, 0x31, 0x41, 0x19, 0x35,
    0, 1, 1, 2, 3, 1,
};

const uint8_t config[] = {
    9, 2, 96, 0, 5, 1, 0, 0x80, 50,
    9, 4, 0, 0, 2, 0xFF, 0, 1, 0,
    7, 5, 0x81, 3, 16, 0, 1,
    7, 5, 0x01, 3, 16, 0, 1,
    // XInput security/control interface: no interrupt endpoint. A host
    // claim failure here must not reject the composite parent device.
    9, 4, 1, 0, 0, 0xFF, 0, 0, 0,
    9, 4, 2, 0, 1, 3, 1, 2, 0,
    7, 5, 0x82, 3, 8, 0, 1,
    9, 4, 3, 0, 1, 3, 1, 2, 0,
    7, 5, 0x83, 3, 8, 0, 1,
    9, 4, 4, 0, 2, 0xFF, 0, 1, 0,
    7, 5, 0x84, 3, 16, 0, 1,
    7, 5, 0x02, 3, 16, 0, 1,
};

bool append(ProfileStore &store, ProfileBlobType type, const uint8_t *data,
            uint16_t length) {
    ProfileBlobChunkHeader header;
    header.type = type;
    header.totalLength = length;
    header.totalCrc = makcm::bridge::crc16Ccitt(data, length);
    header.offset = 0;
    return store.appendProfileBlob(header, data, length);
}
}

int main() {
    DeviceProfile profile;
    profile.kind = DeviceKind::RawUsb;
    profile.vid = 0x4131;
    profile.pid = 0x3519;
    profile.usbSpeed = UsbSpeed::Full;
    profile.deviceDescriptorLength = sizeof(device);
    profile.configurationDescriptorLength = sizeof(config);
    profile.endpointCount = 6;
    profile.blobCount = 2;
    profile.inEndpoint = 0x81;
    profile.configAttributes = 0x80;

    ProfileStore store;
    assert(store.begin(profile));
    assert(append(store, ProfileBlobType::Device, device, sizeof(device)));
    assert(append(store, ProfileBlobType::Configuration, config,
                  sizeof(config)));

    CapabilityInput input{device, sizeof(device), config, sizeof(config),
                          UsbSpeed::Full, 4000000};
    store.setCapabilityResult(makcm::bridge::planCapability(input));
    const auto validation = store.validate();
    assert(validation.ok);
    assert(validation.endpoint_count == 6);
    assert(validation.in_count == 4 && validation.out_count == 2);
    return 0;
}
