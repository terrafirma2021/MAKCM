#include "input_policy.h"

#include <cassert>

using makcm::bridge::DeviceKind;
using makcm::bridge::DeviceProfile;
using makcm::bridge::UsbSpeed;
using makcm::native::InputPolicy;
using makcm::native::ProfileStore;

namespace {
ProfileStore profileStore(uint16_t vid, uint16_t pid) {
    DeviceProfile profile;
    profile.kind = DeviceKind::RawUsb;
    profile.vid = vid;
    profile.pid = pid;
    profile.usbSpeed = UsbSpeed::Full;
    profile.deviceDescriptorLength = 18;
    profile.configurationDescriptorLength = 9;
    ProfileStore store;
    assert(store.begin(profile));
    return store;
}
}

int main() {
    InputPolicy policy;
    auto unknown = profileStore(0x1234, 0x5678);
    policy.configure(&unknown);
    makcm::hid::MouseReportLayout layout;
    assert(!policy.fallbackMouseLayout(layout));
    uint8_t next = 0;
    const uint8_t unknownReport[] = {0, 6, 0, 0, 30, 30};
    assert(!policy.decodeControllerButtons(unknownReport,
                                           sizeof(unknownReport), 0, next));

    auto controller = profileStore(0x4131, 0x3519);
    // The test profile has no legacy endpoint metadata, so the policy does
    // not accidentally classify an arbitrary endpoint as the controller.
    policy.configure(&controller);
    assert(!policy.isControllerEndpoint(0x81));
    assert(policy.fallbackMouseLayout(layout));
    assert(layout.valid && layout.reportId == 0x04 && layout.reportLength == 7);
    assert(policy.decodeControllerButtons(unknownReport,
                                          sizeof(unknownReport), 0, next));
    assert(next == 0x03);

    auto dse = profileStore(0x054C, 0x0DF2);
    policy.configure(&dse);
    uint8_t dseReport[64] = {};
    dseReport[0] = 0x01;
    dseReport[5] = 30;
    dseReport[6] = 30;
    assert(policy.decodeControllerButtons(dseReport, sizeof(dseReport), 0,
                                          next));
    assert(next == 0x03);
    return 0;
}
