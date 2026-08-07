#include "profile_store.h"

#include <cassert>
#include <cstdint>

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
    9, 2, 25, 0, 1, 1, 0, 0x80, 50,
    9, 4, 0, 0, 1, 0xFF, 0, 0, 0,
    7, 5, 0x81, 3, 8, 0, 1,
};

DeviceProfile profile() {
    DeviceProfile value;
    value.kind = DeviceKind::RawUsb;
    value.usbSpeed = UsbSpeed::Full;
    value.vid = 0x4131;
    value.pid = 0x3519;
    value.deviceDescriptorLength = sizeof(device);
    value.configurationDescriptorLength = sizeof(config);
    value.endpointCount = 1;
    value.blobCount = 2;
    value.inEndpoint = 0x81;
    return value;
}

bool append(ProfileStore &store, ProfileBlobType type, const uint8_t *data,
            uint16_t length, uint16_t split, uint16_t crc) {
    ProfileBlobChunkHeader header;
    header.type = type;
    header.totalLength = length;
    header.totalCrc = crc;
    const uint16_t first = split < length ? split : length;
    header.offset = 0;
    if (!store.appendProfileBlob(header, data, first)) return false;
    if (first != length) {
        header.offset = first;
        if (!store.appendProfileBlob(header, data + first,
                                     static_cast<uint16_t>(length - first)))
            return false;
    }
    return true;
}
}

int main() {
    ProfileStore store;
    const DeviceProfile p = profile();
    assert(store.begin(p));
    assert(append(store, ProfileBlobType::Device, device, sizeof(device), 5,
                  makcm::bridge::crc16Ccitt(device, sizeof(device))));
    assert(append(store, ProfileBlobType::Configuration, config, sizeof(config),
                  10, makcm::bridge::crc16Ccitt(config, sizeof(config))));
    makcm::bridge::CapabilityResult validCapability;
    validCapability.capability = makcm::bridge::ProxyCapability::FullProxy;
    validCapability.reason = makcm::bridge::CapabilityReason::None;
    validCapability.speed = UsbSpeed::Full;
    validCapability.configurationCount = 1;
    validCapability.interfaceCount = 1;
    validCapability.endpointCount = 1;
    validCapability.inCount = 1;
    validCapability.outCount = 0;
    validCapability.maxEndpointNumber = 1;
    validCapability.maxPacketSize = 8;
    validCapability.estimatedBitsPerSecond = 64000;
    store.setCapabilityResult(validCapability);
    const auto validation = store.validate();
    assert(validation.ok);
    assert(store.blobCount() == 2);

    ProfileStore bad;
    assert(bad.begin(p));
    assert(append(bad, ProfileBlobType::Device, device, sizeof(device), 9,
                  0x1234));
    assert(append(bad, ProfileBlobType::Configuration, config, sizeof(config),
                  sizeof(config), makcm::bridge::crc16Ccitt(config, sizeof(config))));
    bad.setCapabilityResult(validCapability);
    const auto rejected = bad.validate();
    assert(!rejected.ok && rejected.error_code == 47);

    // A right-side capability rejection is a hard D+ gate.  The left must
    // report the reason without trying to install a partial descriptor set.
    ProfileStore unsupported;
    assert(unsupported.begin(p));
    makcm::bridge::CapabilityResult capability;
    capability.capability = makcm::bridge::ProxyCapability::Unsupported;
    capability.reason = makcm::bridge::CapabilityReason::HighSpeedOnly;
    unsupported.setCapabilityResult(capability);
    const auto rejectedCapability = unsupported.validate();
    assert(!rejectedCapability.ok && rejectedCapability.error_code == 50 &&
           rejectedCapability.capability_reason ==
               static_cast<uint8_t>(
                   makcm::bridge::CapabilityReason::HighSpeedOnly));

    // A BOS blob must include its complete wTotalLength chain, not only the
    // five-byte BOS header.
    ProfileStore bosStore;
    DeviceProfile bosProfile = p;
    bosProfile.blobCount = 3;
    assert(bosStore.begin(bosProfile));
    const uint8_t bos[] = {5, 0x0F, 9, 0, 1, 0x10, 2, 0x00, 0x00};
    assert(append(bosStore, ProfileBlobType::Device, device, sizeof(device),
                  sizeof(device), makcm::bridge::crc16Ccitt(device, sizeof(device))));
    assert(append(bosStore, ProfileBlobType::Configuration, config,
                  sizeof(config), sizeof(config),
                  makcm::bridge::crc16Ccitt(config, sizeof(config))));
    assert(append(bosStore, ProfileBlobType::Bos, bos, sizeof(bos), 4,
                  makcm::bridge::crc16Ccitt(bos, sizeof(bos))));
    bosStore.setCapabilityResult(validCapability);
    const auto bosValidation = bosStore.validate();
    assert(bosValidation.ok);

    ProfileStore badBos;
    assert(badBos.begin(bosProfile));
    const uint8_t badBosData[] = {5, 0x0F, 5, 0, 1, 0x10, 2, 0, 0};
    assert(append(badBos, ProfileBlobType::Device, device, sizeof(device),
                  sizeof(device), makcm::bridge::crc16Ccitt(device, sizeof(device))));
    assert(append(badBos, ProfileBlobType::Configuration, config,
                  sizeof(config), sizeof(config),
                  makcm::bridge::crc16Ccitt(config, sizeof(config))));
    assert(append(badBos, ProfileBlobType::Bos, badBosData, sizeof(badBosData),
                  sizeof(badBosData), makcm::bridge::crc16Ccitt(badBosData, sizeof(badBosData))));
    badBos.setCapabilityResult(validCapability);
    assert(!badBos.validate().ok && badBos.validation().error_code == 48);
    return 0;
}
