#include "profile_store.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <vector>

using makcm::bridge::DeviceKind;
using makcm::bridge::DeviceProfile;
using makcm::bridge::CapabilityResult;
using makcm::bridge::ProxyCapability;
using makcm::bridge::UsbSpeed;
using makcm::native::ProfileStore;

namespace {
void appendChunks(ProfileStore &store, bool reverse) {
    const uint8_t device[] = {
        18, 1, 0x00, 0x02, 0, 0, 0, 64, 0x31, 0x41, 0x19, 0x35,
        0x00, 0x01, 1, 2, 3, 1,
    };
    const uint8_t report[] = {
        0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x05, 0x09, 0x19, 0x01,
        0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01,
        0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x03, 0x05, 0x01,
        0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08,
        0x95, 0x02, 0x81, 0x06, 0xC0,
    };
    const uint8_t config[] = {
        9, 2, 66, 0, 2, 1, 0, 0x80, 50,
        9, 4, 0, 0, 2, 0xFF, 0x5D, 1, 0,
        // Vendor/XInput interfaces may use a 0x21 block that is not a HID
        // descriptor.  ProfileStore must preserve it without parsing it as
        // a HID report descriptor.
        9, 0x21, 0x01, 0x02, 0, 0x99, 0x88, 0x77, 0x66,
        7, 5, 0x81, 3, 64, 0, 1,
        7, 5, 0x01, 3, 64, 0, 1,
        // Many real gaming mice use a non-Boot HID interface (subclass and
        // protocol zero) while still declaring a Mouse application
        // collection in the report descriptor.  Detection must follow the
        // report grammar, not this optional interface tuple.
        9, 4, 1, 0, 1, 3, 0, 0, 0,
        9, 0x21, 0x11, 0x01, 0, 1, 0x22,
        static_cast<uint8_t>(sizeof(report)), 0,
        7, 5, 0x83, 3, 7, 0, 1,
    };
    DeviceProfile profile;
    profile.kind = DeviceKind::RawUsb;
    profile.vid = 0x4131;
    profile.pid = 0x3519;
    profile.deviceDescriptorLength = sizeof(device);
    profile.configurationDescriptorLength = sizeof(config);
    profile.reportDescriptorLength = sizeof(report);
    profile.endpointCount = 3;
    profile.usbSpeed = makcm::bridge::UsbSpeed::Full;
    profile.interfaceNumber = 0;
    profile.inEndpoint = 0x81;
    profile.configAttributes = 0x80;
    assert(store.begin(profile));
    assert(store.appendDeviceDescriptor(0, device, 9));
    assert(store.appendDeviceDescriptor(9, device + 9, sizeof(device) - 9));
    if (reverse) {
        assert(store.appendConfigurationDescriptor(32, config + 32,
                                                    sizeof(config) - 32));
        assert(store.appendConfigurationDescriptor(0, config, 32));
    } else {
        assert(store.appendConfigurationDescriptor(0, config,
                                                    sizeof(config)));
    }
    // Duplicate identical chunks are idempotent; conflicting duplicates are
    // rejected before they can corrupt a committed profile.
    assert(store.appendDeviceDescriptor(0, device, 9));
    assert(!store.appendDeviceDescriptor(0, device + 1, 1));
    // The conflicting write above leaves the profile invalid, so restore the
    // original byte through a new profile for the remainder of the test.
    store.reset();
    assert(store.begin(profile));
    assert(store.appendDeviceDescriptor(0, device, sizeof(device)));
    assert(store.appendConfigurationDescriptor(0, config, sizeof(config)));
    assert(store.appendHidReportDescriptor(1, 0, report, sizeof(report)));
    const uint8_t langid[] = {4, 3, 0x09, 0x04};
    const uint8_t productEn[] = {
        10, 3, 'M', 0, 'A', 0, 'K', 0, 'U', 0,
    };
    const uint8_t productJa[] = {
        10, 3, 'M', 0, 'A', 0, 'K', 0, 'U', 0,
    };
    assert(store.appendStringDescriptor(0, 0, 0, langid, sizeof(langid)));
    assert(store.appendStringDescriptor(2, 0x0409, 0, productEn,
                                        sizeof(productEn)));
    assert(store.appendStringDescriptor(2, 0x0411, 0, productJa,
                                        sizeof(productJa)));
    uint16_t stringLength = 0;
    assert(store.stringDescriptor(2, 0x0409, &stringLength) != nullptr &&
           stringLength == sizeof(productEn));
    assert(store.stringDescriptor(2, 0x0411, &stringLength) != nullptr &&
           stringLength == sizeof(productJa));
    const uint8_t cached[] = {0x28, 0x00, 0x00, 0x00};
    makcm::bridge::ProfileControlCacheHeader cache;
    cache.bmRequestType = 0xC0;
    cache.bRequest = 0x55;
    cache.wIndex = 0x0004;
    cache.wLength = sizeof(cached);
    cache.totalLength = sizeof(cached);
    assert(store.appendControlCache(cache, cached, sizeof(cached)));
    CapabilityResult capability;
    capability.capability = ProxyCapability::FullProxy;
    capability.reason = makcm::bridge::CapabilityReason::None;
    capability.speed = UsbSpeed::Full;
    capability.configurationCount = 1;
    capability.interfaceCount = 2;
    capability.endpointCount = 3;
    capability.inCount = 2;
    capability.outCount = 1;
    capability.maxEndpointNumber = 3;
    capability.maxPacketSize = 64;
    capability.estimatedBitsPerSecond = 1080000;
    store.setCapabilityResult(capability);
    const auto *cached_info = store.cachedControl(0xC0, 0x55, 0, 0x0004);
    assert(cached_info != nullptr && cached_info->received_length == sizeof(cached));
    const auto validation = store.validate();
    if (!validation.ok) {
        fprintf(stderr, "profile validation failed: code=%u message=%s\\n",
                validation.error_code, validation.message);
    }
    assert(validation.ok);
    assert(validation.capability == makcm::bridge::ProxyCapability::FullProxy);
    const auto *xinput = store.xinputEndpoint();
    assert(xinput != nullptr && xinput->address == 0x81);
    const auto *mouse = store.mouseEndpoint();
    assert(mouse != nullptr && mouse->address == 0x83);
    assert(mouse->interface_number == 1);
}
} // namespace

int main() {
    ProfileStore store;
    appendChunks(store, false);
    appendChunks(store, true);
    return 0;
}
