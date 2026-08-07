#include "ProxyCapabilityPlanner.h"

#include <cassert>
#include <cstdint>

using makcm::bridge::CapabilityReason;
using makcm::bridge::CapabilityInput;
using makcm::bridge::ProxyCapability;
using makcm::bridge::UsbSpeed;

namespace {
const uint8_t device[] = {
    18, 1, 0x00, 0x02, 0, 0, 0, 64, 0x31, 0x41, 0x19, 0x35,
    0, 1, 1, 2, 3, 1,
};

const uint8_t config[] = {
    9, 2, 32, 0, 2, 1, 0, 0x80, 50,
    9, 4, 0, 0, 1, 0xFF, 0, 1, 0,
    7, 5, 0x81, 3, 64, 0, 1,
    7, 5, 0x01, 3, 64, 0, 1,
};

// The measured controller topology: four interrupt IN endpoints and two
// interrupt OUT endpoints.  This is the resource boundary the proxy must
// accept without introducing a class-specific queue or remapping.
const uint8_t fourInTwoOut[] = {
    9, 2, 87, 0, 4, 1, 0, 0x80, 50,
    9, 4, 0, 0, 2, 0xFF, 0, 1, 0,
    7, 5, 0x81, 3, 16, 0, 1,
    7, 5, 0x01, 3, 16, 0, 1,
    9, 4, 1, 0, 1, 3, 1, 2, 0,
    7, 5, 0x82, 3, 8, 0, 1,
    9, 4, 2, 0, 1, 3, 1, 2, 0,
    7, 5, 0x83, 3, 8, 0, 1,
    9, 4, 3, 0, 2, 0xFF, 0, 1, 0,
    7, 5, 0x84, 3, 16, 0, 1,
    7, 5, 0x02, 3, 16, 0, 1,
};
}

int main() {
    CapabilityInput input{device, sizeof(device), config, sizeof(config),
                          UsbSpeed::Full, 4000000};
    auto result = makcm::bridge::planCapability(input);
    assert(result.capability == ProxyCapability::FullProxy);
    assert(result.endpointCount == 2 && result.inCount == 1 &&
           result.outCount == 1);

    auto high = input;
    high.speed = UsbSpeed::High;
    result = makcm::bridge::planCapability(high);
    assert(result.capability == ProxyCapability::Unsupported);
    assert(result.reason == CapabilityReason::HighSpeedOnly);

    auto unknownSpeed = input;
    unknownSpeed.speed = UsbSpeed::Unknown;
    result = makcm::bridge::planCapability(unknownSpeed);
    assert(result.capability == ProxyCapability::Unsupported);
    assert(result.reason == CapabilityReason::DescriptorMalformed);

    const uint8_t isoc[] = {
        9, 2, 25, 0, 1, 1, 0, 0x80, 50,
        9, 4, 0, 0, 1, 0x01, 0, 0, 0,
        7, 5, 0x81, 1, 64, 0, 1,
    };
    auto iso = input;
    iso.configuration = isoc;
    iso.configurationLength = sizeof(isoc);
    result = makcm::bridge::planCapability(iso);
    assert(result.capability == ProxyCapability::Unsupported);
    assert(result.reason == CapabilityReason::IsochronousUnsupported);

    const uint8_t tooManyIn[] = {
        9, 2, 53, 0, 1, 1, 0, 0x80, 50,
        9, 4, 0, 0, 5, 0xFF, 0, 0, 0,
        7, 5, 0x81, 3, 8, 0, 1,
        7, 5, 0x82, 3, 8, 0, 1,
        7, 5, 0x83, 3, 8, 0, 1,
        7, 5, 0x84, 3, 8, 0, 1,
        7, 5, 0x85, 3, 8, 0, 1,
    };
    auto many = input;
    many.configuration = tooManyIn;
    many.configurationLength = sizeof(tooManyIn);
    result = makcm::bridge::planCapability(many);
    assert(result.capability == ProxyCapability::Unsupported);
    assert(result.reason == CapabilityReason::TooManyInEndpoints);

    auto controller = input;
    controller.configuration = fourInTwoOut;
    controller.configurationLength = sizeof(fourInTwoOut);
    result = makcm::bridge::planCapability(controller);
    assert(result.capability == ProxyCapability::FullProxy);
    assert(result.endpointCount == 6 && result.inCount == 4 &&
           result.outCount == 2 && result.maxEndpointNumber == 4);

    // bConfigurationValue is not required to be one.  The proxy preserves
    // the raw value and must still accept a single valid configuration.
    auto nonOneConfig = input;
    const uint8_t configValueTwo[] = {
        9, 2, 32, 0, 2, 2, 0, 0x80, 50,
        9, 4, 0, 0, 1, 0xFF, 0, 1, 0,
        7, 5, 0x81, 3, 64, 0, 1,
        7, 5, 0x01, 3, 64, 0, 1,
    };
    nonOneConfig.configuration = configValueTwo;
    nonOneConfig.configurationLength = sizeof(configValueTwo);
    result = makcm::bridge::planCapability(nonOneConfig);
    assert(result.capability == ProxyCapability::FullProxy);

    const uint8_t endpointSeven[] = {
        9, 2, 25, 0, 1, 1, 0, 0x80, 50,
        9, 4, 0, 0, 1, 0xFF, 0, 1, 0,
        7, 5, 0x87, 3, 8, 0, 1,
    };
    auto remapRequired = input;
    remapRequired.configuration = endpointSeven;
    remapRequired.configurationLength = sizeof(endpointSeven);
    result = makcm::bridge::planCapability(remapRequired);
    assert(result.capability == ProxyCapability::Unsupported &&
           result.reason == CapabilityReason::EndpointNumberOutOfRange);
    return 0;
}
