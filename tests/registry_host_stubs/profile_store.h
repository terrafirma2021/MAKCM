#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace makcm::native {

struct EndpointInfo {
    uint8_t address = 0;
    uint8_t interface_number = 0;
    uint8_t attributes = 0;
    uint16_t max_packet = 0;
    uint8_t interval = 0;
};

struct HidReportInfo {
    uint8_t interface_number = 0xFF;
    uint16_t expected_length = 0;
    uint16_t received_length = 0;
    const uint8_t *data = nullptr;
};

class ProfileStore final {
public:
    bool ready() const { return ready_; }
    uint16_t deviceDescriptorLength() const { return device_length_; }
    const uint8_t *deviceDescriptor() const { return device_; }
    const uint8_t *configurationDescriptor() const { return configuration_; }
    uint16_t configurationDescriptorLength() const { return configuration_length_; }

    const EndpointInfo *endpoint(uint8_t address) const {
        for (size_t index = 0; index < endpoint_count_; ++index) {
            if (endpoints_[index].address == address) return &endpoints_[index];
        }
        return nullptr;
    }

    const HidReportInfo *hidReport(uint8_t interface_number) const {
        for (size_t index = 0; index < hid_count_; ++index) {
            if (hid_reports_[index].interface_number == interface_number)
                return &hid_reports_[index];
        }
        return nullptr;
    }

    void setDescriptors(const uint8_t *device, uint16_t device_length,
                        const uint8_t *configuration,
                        uint16_t configuration_length) {
        device_ = device;
        device_length_ = device_length;
        configuration_ = configuration;
        configuration_length_ = configuration_length;
        ready_ = true;
    }

    void setEndpoint(uint8_t address, uint8_t interface_number) {
        endpoints_[endpoint_count_++] = {address, interface_number, 0, 0, 0};
    }

    void setHidReport(uint8_t interface_number, const uint8_t *data,
                      uint16_t length) {
        hid_reports_[hid_count_++] = {interface_number, length, length, data};
    }

private:
    bool ready_ = false;
    const uint8_t *device_ = nullptr;
    uint16_t device_length_ = 0;
    const uint8_t *configuration_ = nullptr;
    uint16_t configuration_length_ = 0;
    std::array<EndpointInfo, 8> endpoints_{};
    size_t endpoint_count_ = 0;
    std::array<HidReportInfo, 8> hid_reports_{};
    size_t hid_count_ = 0;
};

} // namespace makcm::native
