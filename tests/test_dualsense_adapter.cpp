#include "DualSenseControllerAdapter.h"
#include "ProxyCapabilityPlanner.h"

#include <cassert>
#include <cstdint>
#include <array>
#include <initializer_list>

using namespace makcm::bridge;

namespace {

struct BlobBuilder {
    std::array<uint8_t, 512> bytes{};
    uint16_t size = 0;

    void append(std::initializer_list<uint8_t> values) {
        for (uint8_t value : values) bytes[size++] = value;
    }

    const uint8_t *data() const { return bytes.data(); }
};

BlobBuilder fixture(uint8_t inEndpoint = 0x83,
                    uint8_t outEndpoint = 0x03) {
    BlobBuilder config;
    config.append({9, 2, 0, 0, 4, 1, 0, 0xC0, 0xFA});
    // Audio control and streaming alt zero remain in the physical profile,
    // but must not appear in the controller-only result.
    config.append({9, 4, 0, 0, 0, 1, 1, 0, 0,
                   9, 0x24, 1, 0, 1, 0, 0, 0, 0});
    config.append({9, 4, 1, 0, 0, 1, 2, 0, 0});
    // A non-zero audio alternate and isochronous endpoint are deliberately
    // present to prove that filtering is selective rather than global reject.
    config.append({9, 4, 1, 1, 1, 1, 2, 0, 0,
                   7, 5, 0x81, 1, 0x40, 0, 1});
    // HID controller interface, with class HID descriptor plus interrupt IN/OUT.
    config.append({9, 4, 2, 0, 2, 3, 0, 0, 3,
                   9, 0x21, 0x11, 0x01, 0, 1, 0x22, 4, 0,
                   7, 5, inEndpoint, 3, 0x40, 0, 1,
                   7, 5, outEndpoint, 3, 0x40, 0, 1});
    const uint16_t total = config.size;
    config.bytes[2] = static_cast<uint8_t>(total);
    config.bytes[3] = static_cast<uint8_t>(total >> 8);
    return config;
}

const uint8_t kDualSenseDevice[] = {
    18, 1, 0, 2, 0, 0, 0, 64, 0x4C, 0x05, 0xE6, 0x0C,
    0, 1, 1, 2, 0, 1,
};

const uint8_t kDualSenseEdgeDevice[] = {
    18, 1, 0, 2, 0, 0, 0, 64, 0x4C, 0x05, 0xF2, 0x0D,
    0, 1, 1, 2, 0, 1,
};

const uint8_t kOtherSonyDevice[] = {
    18, 1, 0, 2, 0, 0, 0, 64, 0x4C, 0x05, 0xCC, 0x09,
    0, 1, 1, 2, 0, 1,
};

} // namespace

int main() {
    assert(isDualSenseControllerDevice(kDualSenseDevice,
                                       sizeof(kDualSenseDevice)));
    assert(isDualSenseControllerDevice(kDualSenseEdgeDevice,
                                       sizeof(kDualSenseEdgeDevice)));
    assert(isDualSenseDevice(kDualSenseEdgeDevice,
                             sizeof(kDualSenseEdgeDevice)));
    assert(!isDualSenseControllerDevice(kOtherSonyDevice,
                                        sizeof(kOtherSonyDevice)));

    auto input = fixture();
    uint8_t output[kMaxConfigurationDescriptorSize]{};
    DualSenseTransformResult transformed;
    assert(transformDualSenseConfiguration(input.data(), input.size, output,
                                           sizeof(output), transformed));
    assert(transformed.transformed);
    assert(transformed.filtered);
    assert(transformed.interfaceCount == 1);
    assert(transformed.endpointCount == 2);
    assert(transformed.inCount == 1 && transformed.outCount == 1);
    assert(transformed.interfaces[0].physical == 2);
    assert(transformed.interfaces[0].proxy == 0);
    assert(dualSenseMapInterface(transformed, 2) == 0);
    assert(dualSenseMapPhysicalInterface(transformed, 0) == 2);
    // Audio interfaces are filtered from the controller-only projection and
    // therefore must never resolve to a physical target.
    assert(dualSenseMapInterface(transformed, 0) == 0xFF);
    assert(dualSenseMapPhysicalInterface(transformed, 1) == 0xFF);
    assert(transformed.endpoints[0].physical == 0x83);
    assert(transformed.endpoints[0].proxy == 0x83);
    assert(transformed.endpoints[1].physical == 0x03);
    assert(transformed.endpoints[1].proxy == 0x03);
    assert(output[4] == 1);
    assert((static_cast<uint16_t>(output[2]) |
            (static_cast<uint16_t>(output[3]) << 8)) ==
           transformed.outputLength);

    CapabilityInput capabilityInput{kDualSenseDevice,
                                    sizeof(kDualSenseDevice), output,
                                    transformed.outputLength, UsbSpeed::Full,
                                    4000000};
    const auto capability = planCapability(capabilityInput);
    assert(capability.capability == ProxyCapability::FullProxy);
    assert(capability.endpointCount == 2);

    // The Edge uses the same controller HID projection.  Its separate
    // trigger policy is selected by the right host's 054c:0df2 check, while
    // the shared descriptor transformer and capability plan stay identical.
    input = fixture();
    assert(transformDualSenseConfiguration(input.data(), input.size, output,
                                           sizeof(output), transformed));
    CapabilityInput edgeCapabilityInput{kDualSenseEdgeDevice,
                                        sizeof(kDualSenseEdgeDevice), output,
                                        transformed.outputLength, UsbSpeed::Full,
                                        4000000};
    const auto edgeCapability = planCapability(edgeCapabilityInput);
    assert(edgeCapability.capability == ProxyCapability::FullProxy);
    assert(edgeCapability.endpointCount == 2);

    // Endpoints above six are remapped in the proxy descriptor while the
    // physical address remains available to the host-side adapter.
    input = fixture(0x87, 0x07);
    assert(transformDualSenseConfiguration(input.data(), input.size, output,
                                           sizeof(output), transformed));
    assert(transformed.endpoints[0].physical == 0x87);
    assert(transformed.endpoints[0].proxy == 0x81);
    assert(transformed.endpoints[1].physical == 0x07);
    assert(transformed.endpoints[1].proxy == 0x01);
    assert(dualSenseMapPhysicalEndpoint(transformed, 0x81) == 0x87);
    assert(dualSenseMapPhysicalEndpoint(transformed, 0x01) == 0x07);

    // The checked-in PS5 fixture advertises wTotalLength=0xE3 but is shorter;
    // a truncated host-cache entry must never be transformed.
    const uint8_t truncated[] = {9, 2, 0xE3, 0, 4, 1, 0, 0xC0, 0xFA};
    assert(!transformDualSenseConfiguration(truncated, sizeof(truncated),
                                            output, sizeof(output), transformed));
    assert(transformed.reason == CapabilityReason::BlobIncomplete);
    return 0;
}
