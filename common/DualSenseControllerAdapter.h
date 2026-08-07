#pragma once

#include <stdint.h>
#include <string.h>

#include "BridgeProtocol.h"

// The normal USB proxy deliberately rejects alternate settings and
// isochronous endpoints.  Wired DualSense/DualSense Edge controllers
// (054c:0ce6 / 054c:0df2) commonly advertise an audio function alongside
// their HID controller function.  This adapter is intentionally narrow: it
// derives a controller-only configuration from the complete downstream blob
// and records the address mappings needed to claim/forward the original HID
// interface.  Edge-specific trigger policy remains separate in the right
// host; this header handles the shared USB persona transformation only.
namespace makcm::bridge {

constexpr uint16_t kDualSenseUsbVid = 0x054C;
constexpr uint16_t kDualSenseUsbPid = 0x0CE6;
constexpr uint16_t kDualSenseEdgeUsbPid = 0x0DF2;

// Compatibility aliases for callers that used the original standard-
// DualSense names. `isDualSenseControllerDevice` is authoritative now that
// both USB personas share the controller-only adapter.
constexpr uint16_t kDualSenseVid = kDualSenseUsbVid;
constexpr uint16_t kDualSensePid = kDualSenseUsbPid;

constexpr uint8_t kDualSenseMaxInterfaces = 8;
constexpr uint8_t kDualSenseMaxEndpoints = 6;

struct DualSenseInterfaceMap {
    uint8_t physical = 0xFF;
    uint8_t proxy = 0xFF;
};

struct DualSenseEndpointMap {
    uint8_t physical = 0;
    uint8_t proxy = 0;
    uint8_t interfacePhysical = 0xFF;
    uint8_t interfaceProxy = 0xFF;
};

struct DualSenseTransformResult {
    bool transformed = false;
    bool filtered = false;
    CapabilityReason reason = CapabilityReason::None;
    uint16_t outputLength = 0;
    uint8_t interfaceCount = 0;
    uint8_t endpointCount = 0;
    uint8_t inCount = 0;
    uint8_t outCount = 0;
    DualSenseInterfaceMap interfaces[kDualSenseMaxInterfaces]{};
    DualSenseEndpointMap endpoints[kDualSenseMaxEndpoints]{};
};

inline bool isDualSenseControllerPid(uint16_t pid) {
    return pid == kDualSenseUsbPid || pid == kDualSenseEdgeUsbPid;
}

inline bool isDualSenseControllerDevice(const uint8_t *device,
                                        uint16_t length) {
    return device != nullptr && length >= 18 && device[0] >= 18 &&
           device[1] == 0x01 &&
           (static_cast<uint16_t>(device[8]) |
            (static_cast<uint16_t>(device[9]) << 8)) == kDualSenseUsbVid &&
           isDualSenseControllerPid(
               static_cast<uint16_t>(device[10]) |
               (static_cast<uint16_t>(device[11]) << 8));
}

// Keep the original spelling source-compatible for existing integrations; it
// now intentionally includes the Edge PID as well.
inline bool isDualSenseDevice(const uint8_t *device, uint16_t length) {
    return isDualSenseControllerDevice(device, length);
}

inline uint16_t dualSenseLe16(const uint8_t *p) {
    return static_cast<uint16_t>(p[0]) |
           (static_cast<uint16_t>(p[1]) << 8);
}

inline void dualSensePutLe16(uint8_t *p, uint16_t value) {
    p[0] = static_cast<uint8_t>(value);
    p[1] = static_cast<uint8_t>(value >> 8);
}

inline uint8_t dualSenseEndpointNumber(uint8_t address) {
    return static_cast<uint8_t>(address & 0x0F);
}

inline bool dualSenseEndpointIn(uint8_t address) {
    return (address & 0x80) != 0;
}

inline uint8_t dualSenseFindEndpoint(const DualSenseTransformResult &result,
                                     uint8_t address) {
    for (uint8_t i = 0; i < result.endpointCount; ++i) {
        if (result.endpoints[i].proxy == address) return i;
    }
    return 0xFF;
}

inline uint8_t dualSenseMapEndpoint(const DualSenseTransformResult &result,
                                    uint8_t physical) {
    for (uint8_t i = 0; i < result.endpointCount; ++i) {
        if (result.endpoints[i].physical == physical)
            return result.endpoints[i].proxy;
    }
    return 0;
}

inline uint8_t dualSenseMapPhysicalEndpoint(
    const DualSenseTransformResult &result, uint8_t proxy) {
    for (uint8_t i = 0; i < result.endpointCount; ++i) {
        if (result.endpoints[i].proxy == proxy)
            return result.endpoints[i].physical;
    }
    return 0;
}

inline uint8_t dualSenseMapInterface(const DualSenseTransformResult &result,
                                     uint8_t physical) {
    for (uint8_t i = 0; i < result.interfaceCount; ++i) {
        if (result.interfaces[i].physical == physical)
            return result.interfaces[i].proxy;
    }
    return 0xFF;
}

inline uint8_t dualSenseMapPhysicalInterface(
    const DualSenseTransformResult &result, uint8_t proxy) {
    for (uint8_t i = 0; i < result.interfaceCount; ++i) {
        if (result.interfaces[i].proxy == proxy)
            return result.interfaces[i].physical;
    }
    return 0xFF;
}

// Build a controller-only configuration from a complete configuration blob.
// The input must contain exactly wTotalLength bytes; a short host-cache entry
// is never treated as a valid profile.  Every retained interface is HID,
// alternate setting zero.  Non-HID interfaces (including Audio) and all
// non-interrupt endpoints are omitted.  The HID class/report descriptors and
// string indexes are copied byte-for-byte.  Interface and endpoint addresses
// are compacted only when necessary, with mappings returned in `result`.
inline bool transformDualSenseConfiguration(
    const uint8_t *input, uint16_t inputLength, uint8_t *output,
    uint16_t outputCapacity, DualSenseTransformResult &result) {
    result = {};
    result.reason = CapabilityReason::DescriptorMalformed;
    if (input == nullptr || output == nullptr || inputLength < 9 ||
        outputCapacity < 9 || input[0] != 9 || input[1] != 0x02 ||
        dualSenseLe16(input + 2) != inputLength) {
        result.reason = CapabilityReason::BlobIncomplete;
        return false;
    }

    memcpy(output, input, 9);
    uint16_t outputOffset = 9;
    uint16_t inputOffset = 9;
    bool sawHid = false;
    bool sawFiltered = false;
    bool physicalEndpointSeen[256]{};
    bool proxyEndpointSeen[256]{};

    while (inputOffset < inputLength) {
        if (inputOffset + 2 > inputLength) {
            result.reason = CapabilityReason::DescriptorMalformed;
            return false;
        }
        const uint8_t length = input[inputOffset];
        const uint8_t type = input[inputOffset + 1];
        if (length < 2 || static_cast<uint32_t>(inputOffset) + length >
                              inputLength) {
            result.reason = CapabilityReason::DescriptorMalformed;
            return false;
        }
        if (type != 0x04) {
            // Configuration-level descriptors belong to a discarded function
            // unless they occur inside a retained HID interface group.
            inputOffset = static_cast<uint16_t>(inputOffset + length);
            continue;
        }
        if (length < 9) {
            result.reason = CapabilityReason::DescriptorMalformed;
            return false;
        }

        const uint8_t physicalInterface = input[inputOffset + 2];
        const uint8_t alternate = input[inputOffset + 3];
        const uint8_t interfaceClass = input[inputOffset + 5];
        uint16_t groupEnd = static_cast<uint16_t>(inputOffset + length);
        while (groupEnd < inputLength) {
            if (groupEnd + 2 > inputLength) {
                result.reason = CapabilityReason::DescriptorMalformed;
                return false;
            }
            if (input[groupEnd + 1] == 0x04) break;
            const uint8_t childLength = input[groupEnd];
            if (childLength < 2 ||
                static_cast<uint32_t>(groupEnd) + childLength > inputLength) {
                result.reason = CapabilityReason::DescriptorMalformed;
                return false;
            }
            groupEnd = static_cast<uint16_t>(groupEnd + childLength);
        }
        if (groupEnd > inputLength) {
            result.reason = CapabilityReason::DescriptorMalformed;
            return false;
        }

        const bool keepInterface = alternate == 0 && interfaceClass == 0x03;
        if (!keepInterface) {
            if (alternate != 0 || interfaceClass != 0x03)
                sawFiltered = true;
            inputOffset = groupEnd;
            continue;
        }
        sawHid = true;
        if (result.interfaceCount >= kDualSenseMaxInterfaces) {
            result.reason = CapabilityReason::TooManyEndpoints;
            return false;
        }
        if (outputOffset + length > outputCapacity) {
            result.reason = CapabilityReason::BlobIncomplete;
            return false;
        }

        const uint16_t interfaceOutputOffset = outputOffset;
        memcpy(output + outputOffset, input + inputOffset, length);
        output[outputOffset + 2] = result.interfaceCount;
        output[outputOffset + 3] = 0;
        output[outputOffset + 4] = 0;
        const uint8_t proxyInterface = result.interfaceCount;
        ++result.interfaceCount;
        outputOffset = static_cast<uint16_t>(outputOffset + length);
        uint8_t retainedEndpoints = 0;

        uint16_t child = static_cast<uint16_t>(inputOffset + length);
        while (child < groupEnd) {
            const uint8_t childLength = input[child];
            const uint8_t childType = input[child + 1];
            if (childType == 0x05) {
                if (childLength < 7) {
                    result.reason = CapabilityReason::DescriptorMalformed;
                    return false;
                }
                const uint8_t physicalAddress = input[child + 2];
                const uint8_t transferType = input[child + 3] & 0x03;
                const uint16_t rawMps = dualSenseLe16(input + child + 4);
                const uint16_t mps = rawMps & 0x07FF;
                if (transferType != 3 || (rawMps & 0x1800) != 0 ||
                    mps == 0 || mps > 64) {
                    sawFiltered = true;
                    child = static_cast<uint16_t>(child + childLength);
                    continue;
                }
                if (physicalEndpointSeen[physicalAddress] ||
                    result.endpointCount >= kDualSenseMaxEndpoints) {
                    result.reason = CapabilityReason::TooManyEndpoints;
                    return false;
                }
                uint8_t number = dualSenseEndpointNumber(physicalAddress);
                uint8_t proxyAddress = physicalAddress;
                if (number == 0 || number > 6 ||
                    proxyEndpointSeen[proxyAddress]) {
                    proxyAddress = 0;
                    for (uint8_t candidate = 1; candidate <= 6; ++candidate) {
                        const uint8_t candidateAddress = static_cast<uint8_t>(
                            (physicalAddress & 0x80) | candidate);
                        if (!proxyEndpointSeen[candidateAddress]) {
                            proxyAddress = candidateAddress;
                            break;
                        }
                    }
                    if (proxyAddress == 0) {
                        result.reason = CapabilityReason::EndpointNumberOutOfRange;
                        return false;
                    }
                }
                if (outputOffset + childLength > outputCapacity) {
                    result.reason = CapabilityReason::BlobIncomplete;
                    return false;
                }
                memcpy(output + outputOffset, input + child, childLength);
                output[outputOffset + 2] = proxyAddress;
                proxyEndpointSeen[proxyAddress] = true;
                physicalEndpointSeen[physicalAddress] = true;
                DualSenseEndpointMap &map = result.endpoints[result.endpointCount++];
                map.physical = physicalAddress;
                map.proxy = proxyAddress;
                map.interfacePhysical = physicalInterface;
                map.interfaceProxy = proxyInterface;
                if (dualSenseEndpointIn(proxyAddress)) ++result.inCount;
                else ++result.outCount;
                ++retainedEndpoints;
                outputOffset = static_cast<uint16_t>(outputOffset + childLength);
            } else {
                if (outputOffset + childLength > outputCapacity) {
                    result.reason = CapabilityReason::BlobIncomplete;
                    return false;
                }
                memcpy(output + outputOffset, input + child, childLength);
                outputOffset = static_cast<uint16_t>(outputOffset + childLength);
            }
            child = static_cast<uint16_t>(child + childLength);
        }

        if (retainedEndpoints == 0) {
            // A HID control-only/iso-only alternate cannot provide the
            // controller transport; remove the provisional interface/group.
            outputOffset = interfaceOutputOffset;
            --result.interfaceCount;
            for (uint8_t i = 0; i < result.endpointCount; ++i) {
                if (result.endpoints[i].interfaceProxy == proxyInterface) {
                    proxyEndpointSeen[result.endpoints[i].proxy] = false;
                    physicalEndpointSeen[result.endpoints[i].physical] = false;
                    if (dualSenseEndpointIn(result.endpoints[i].proxy))
                        --result.inCount;
                    else
                        --result.outCount;
                    for (uint8_t j = i + 1; j < result.endpointCount; ++j)
                        result.endpoints[j - 1] = result.endpoints[j];
                    --result.endpointCount;
                    --i;
                }
            }
            sawFiltered = true;
        } else {
            output[interfaceOutputOffset + 4] = retainedEndpoints;
            result.interfaces[result.interfaceCount - 1].physical =
                physicalInterface;
            result.interfaces[result.interfaceCount - 1].proxy = proxyInterface;
        }
        inputOffset = groupEnd;
    }

    if (!sawHid || result.interfaceCount == 0 || result.endpointCount == 0 ||
        result.inCount == 0) {
        result.reason = CapabilityReason::DescriptorMalformed;
        return false;
    }
    dualSensePutLe16(output + 2, outputOffset);
    output[4] = result.interfaceCount;
    result.outputLength = outputOffset;
    result.filtered = sawFiltered;
    result.transformed = true;
    result.reason = CapabilityReason::None;
    return true;
}

} // namespace makcm::bridge
