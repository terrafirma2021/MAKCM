#pragma once

#include <stdint.h>

namespace makcm::usb {

// USB HID 1.11 class values not defined by ESP-IDF's public Chapter 9 types.
inline constexpr uint8_t kHidDescriptorType = 0x21;
inline constexpr uint8_t kHidBootSubclass = 0x01;
inline constexpr uint8_t kHidMouseProtocol = 0x02;
inline constexpr uint8_t kMouseButtonLeft = 0x01;
inline constexpr uint8_t kMouseButtonRight = 0x02;
inline constexpr uint8_t kMouseButtonMiddle = 0x04;
inline constexpr uint8_t kMouseButtonBackward = 0x08;
inline constexpr uint8_t kMouseButtonForward = 0x10;

struct __attribute__((packed)) HidClassDescriptor {
    uint8_t length;
    uint8_t descriptorType;
    uint16_t hidVersion;
    uint8_t countryCode;
    uint8_t descriptorCount;
    uint8_t reportDescriptorType;
    uint16_t reportDescriptorLength;
};

// HID 1.11 boot mouse prefix plus the optional horizontal pan byte used by
// the existing report path.
struct __attribute__((packed)) BootMouseReport {
    uint8_t buttons;
    int8_t x;
    int8_t y;
    int8_t wheel;
    int8_t pan;
};

static_assert(sizeof(HidClassDescriptor) == 9);
static_assert(sizeof(BootMouseReport) == 5);

} // namespace makcm::usb
