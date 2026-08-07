#pragma once

#include <algorithm>
#include <stdint.h>

#include "BridgeProtocol.h"

namespace makcm::bridge {

struct CapabilityInput {
    const uint8_t *device = nullptr;
    uint16_t deviceLength = 0;
    const uint8_t *configuration = nullptr;
    uint16_t configurationLength = 0;
    UsbSpeed speed = UsbSpeed::Unknown;
    uint32_t uartBudgetBitsPerSecond = 4000000;
};

inline uint16_t capabilityLe16(const uint8_t *p) {
    return static_cast<uint16_t>(p[0]) |
           (static_cast<uint16_t>(p[1]) << 8);
}

inline CapabilityResult planCapability(const CapabilityInput &input) {
    CapabilityResult result{};
    result.speed = input.speed;
    auto reject = [&](CapabilityReason reason) {
        result.capability = ProxyCapability::Unsupported;
        result.reason = reason;
        return result;
    };

    if (input.device == nullptr || input.deviceLength < 18 ||
        input.configuration == nullptr || input.configurationLength < 9) {
        return reject(CapabilityReason::DescriptorMalformed);
    }
    if (input.speed == UsbSpeed::Unknown) {
        return reject(CapabilityReason::DescriptorMalformed);
    }
    if (input.speed == UsbSpeed::High ||
        capabilityLe16(input.device + 2) > 0x0200) {
        return reject(CapabilityReason::HighSpeedOnly);
    }
    if (input.device[7] != 64) {
        return reject(CapabilityReason::Ep0PacketSizeUnsupported);
    }
    if (input.device[4] == 0x09) {
        return reject(CapabilityReason::HubDevice);
    }
    if (input.device[17] != 1) {
        return reject(CapabilityReason::MultipleConfigurations);
    }
    result.configurationCount = 1;

    const uint8_t *config = input.configuration;
    const uint16_t total = capabilityLe16(config + 2);
    // A device may legally use any non-zero bConfigurationValue.  The
    // single-configuration boundary is enforced from bNumConfigurations in
    // the device descriptor; hard-coding value 1 would reject a valid raw
    // descriptor and force an unnecessary rewrite.
    if (config[0] != 9 || config[1] != 2 || total != input.configurationLength ||
        total < 9 || config[4] == 0 || config[5] == 0) {
        return reject(CapabilityReason::DescriptorMalformed);
    }

    bool seenEndpoint[256]{};
    bool seenInterface[256]{};
    bool hasBulk = false;
    uint8_t currentInterface = 0xFF;
    uint8_t currentAlternate = 0xFF;
    for (uint16_t offset = 0; offset < total;) {
        if (offset + 2 > total) return reject(CapabilityReason::DescriptorMalformed);
        const uint8_t length = config[offset];
        const uint8_t type = config[offset + 1];
        if (length < 2 || static_cast<uint32_t>(offset) + length > total)
            return reject(CapabilityReason::DescriptorMalformed);
        const uint8_t *desc = config + offset;
        if (type == 4) {
            if (length < 9) return reject(CapabilityReason::DescriptorMalformed);
            currentInterface = desc[2];
            currentAlternate = desc[3];
            if (currentAlternate != 0)
                return reject(CapabilityReason::AlternateSettingUnsupported);
            if (seenInterface[currentInterface])
                return reject(CapabilityReason::AlternateSettingUnsupported);
            seenInterface[currentInterface] = true;
            ++result.interfaceCount;
            if (desc[5] == 0x09)
                return reject(CapabilityReason::HubDevice);
        } else if (type == 5) {
            if (length < 7 || currentInterface == 0xFF ||
                currentAlternate != 0)
                return reject(CapabilityReason::DescriptorMalformed);
            const uint8_t address = desc[2];
            const uint8_t number = address & 0x0F;
            const uint16_t rawMps = capabilityLe16(desc + 4);
            const uint16_t mps = rawMps & 0x07FF;
            const uint8_t transfer = desc[3] & 0x03;
            if (seenEndpoint[address])
                return reject(CapabilityReason::DescriptorMalformed);
            // ProxyWithEndpointRemap is intentionally not advertised yet:
            // without a descriptor rewrite plus an original<->virtual
            // address table, accepting endpoint 7+ would silently misroute
            // traffic. Reject it instead of claiming a partial remap.
            if (number == 0 || number > 6)
                return reject(CapabilityReason::EndpointNumberOutOfRange);
            if ((rawMps & 0x1800) != 0 || mps == 0 || mps > 64)
                return reject(CapabilityReason::PacketSizeUnsupported);
            if (transfer == 1)
                return reject(CapabilityReason::IsochronousUnsupported);
            if (transfer != 2 && transfer != 3)
                return reject(CapabilityReason::DescriptorMalformed);
            seenEndpoint[address] = true;
            ++result.endpointCount;
            result.maxEndpointNumber = std::max(result.maxEndpointNumber, number);
            result.maxPacketSize = std::max(result.maxPacketSize, mps);
            if ((address & 0x80) != 0) ++result.inCount;
            else ++result.outCount;
            if (transfer == 2) hasBulk = true;
            const uint32_t interval = std::max<uint8_t>(desc[6], 1);
            result.estimatedBitsPerSecond +=
                static_cast<uint32_t>(mps) * 8u * 1000u / interval;
        }
        offset = static_cast<uint16_t>(offset + length);
    }

    if (result.endpointCount > 6)
        return reject(CapabilityReason::TooManyEndpoints);
    if (result.inCount > 4)
        return reject(CapabilityReason::TooManyInEndpoints);
    if (result.outCount > 4)
        return reject(CapabilityReason::TooManyOutEndpoints);
    if (result.estimatedBitsPerSecond > input.uartBudgetBitsPerSecond)
        return reject(CapabilityReason::UartBudgetExceeded);

    result.capability = hasBulk ? ProxyCapability::LimitedProxy
                                : ProxyCapability::FullProxy;
    result.reason = CapabilityReason::None;
    return result;
}

inline const char *capabilityReasonText(CapabilityReason reason) {
    switch (reason) {
    case CapabilityReason::None: return "none";
    case CapabilityReason::HighSpeedOnly: return "high_speed_only";
    case CapabilityReason::HubDevice: return "hub_device";
    case CapabilityReason::MultipleConfigurations: return "multiple_configurations";
    case CapabilityReason::TooManyEndpoints: return "too_many_endpoints";
    case CapabilityReason::TooManyInEndpoints: return "too_many_in_endpoints";
    case CapabilityReason::TooManyOutEndpoints: return "too_many_out_endpoints";
    case CapabilityReason::EndpointNumberOutOfRange: return "endpoint_number";
    case CapabilityReason::IsochronousUnsupported: return "isochronous";
    case CapabilityReason::AlternateSettingUnsupported: return "alternate_setting";
    case CapabilityReason::PacketSizeUnsupported: return "packet_size";
    case CapabilityReason::Ep0PacketSizeUnsupported: return "ep0_packet_size";
    case CapabilityReason::DescriptorMalformed: return "descriptor_malformed";
    case CapabilityReason::UartBudgetExceeded: return "uart_budget";
    case CapabilityReason::BlobIncomplete: return "blob_incomplete";
    case CapabilityReason::BlobCrcMismatch: return "blob_crc";
    }
    return "unknown";
}

} // namespace makcm::bridge
