#pragma once

#include <stddef.h>
#include <stdint.h>

#include <array>

#include "BridgeProtocol.h"

namespace makcm::native {

struct ProfileValidation {
    bool ok = false;
    uint8_t error_code = 0;
    uint8_t interface_count = 0;
    uint8_t endpoint_count = 0;
    uint8_t in_count = 0;
    uint8_t out_count = 0;
    makcm::bridge::ProxyCapability capability =
        makcm::bridge::ProxyCapability::Unsupported;
    uint8_t capability_reason = 0;
    uint16_t max_packet = 0;
    uint8_t blob_count = 0;
    uint16_t max_endpoint_number = 0;
    uint32_t estimated_bits_per_second = 0;
    char message[96] = {};
};

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

struct CachedControlInfo {
    bool used = false;
    uint8_t bm_request_type = 0;
    uint8_t b_request = 0;
    uint16_t w_value = 0;
    uint16_t w_index = 0;
    uint16_t w_length = 0;
    uint16_t received_length = 0;
    const uint8_t *data = nullptr;
};

struct ProfileBlobInfo {
    bool used = false;
    makcm::bridge::ProfileBlobType type =
        makcm::bridge::ProfileBlobType::Unknown;
    uint16_t key = 0;
    uint16_t langid = 0;
    uint16_t length = 0;
    uint16_t crc = 0;
    const uint8_t *data = nullptr;
};

// Owns one complete protocol-v5 profile.  It is deliberately independent of
// TinyUSB/FreeRTOS so it can be fuzzed and unit-tested on the host.
class ProfileStore final {
public:
    ProfileStore();

    void reset();
    bool begin(const makcm::bridge::DeviceProfile &profile);
    bool appendDeviceDescriptor(uint16_t offset, const uint8_t *data,
                                uint16_t length);
    bool appendConfigurationDescriptor(uint16_t offset, const uint8_t *data,
                                       uint16_t length);
    bool appendHidReportDescriptor(uint8_t interface_number, uint16_t offset,
                                   const uint8_t *data, uint16_t length);
    bool appendStringDescriptor(uint8_t index, uint16_t langid, uint16_t offset,
                                const uint8_t *data, uint16_t length);
    bool appendProfileBlob(
        const makcm::bridge::ProfileBlobChunkHeader &header,
        const uint8_t *data, uint16_t length);
    bool appendControlCache(
        const makcm::bridge::ProfileControlCacheHeader &header,
        const uint8_t *data, uint16_t length);
    void setCapabilityResult(const makcm::bridge::CapabilityResult &result) {
        capability_result_ = result;
        capability_result_received_ = true;
    }
    const makcm::bridge::CapabilityResult &capabilityResult() const {
        return capability_result_;
    }
    bool capabilityResultReceived() const {
        return capability_result_received_;
    }

    ProfileValidation validate();
    bool ready() const { return validated_; }
    const ProfileValidation &validation() const { return validation_; }
    makcm::bridge::ProxyCapability capability() const {
        return validation_.capability;
    }

    const makcm::bridge::DeviceProfile &profile() const { return profile_; }
    const uint8_t *deviceDescriptor() const { return device_.data(); }
    uint16_t deviceDescriptorLength() const { return device_length_; }
    const uint8_t *configurationDescriptor() const { return configuration_.data(); }
    uint16_t configurationDescriptorLength() const { return configuration_length_; }
    const uint8_t *bosDescriptor() const;
    uint16_t bosDescriptorLength() const;
    const uint8_t *qualifierDescriptor() const;
    uint16_t qualifierDescriptorLength() const;
    uint8_t blobCount() const { return blob_count_; }
    const ProfileBlobInfo *blob(uint8_t index) const;

    const EndpointInfo *endpoints() const { return endpoints_.data(); }
    uint8_t endpointCount() const { return endpoint_count_; }
    const EndpointInfo *endpoint(uint8_t address) const;
    const EndpointInfo *mouseEndpoint() const;
    const EndpointInfo *xinputEndpoint() const;

    uint8_t hidReportCount() const { return hid_count_; }
    const HidReportInfo *hidReports() const { return hid_reports_.data(); }
    const HidReportInfo *hidReport(uint8_t interface_number) const;

    // Returns the exact raw UTF-16 descriptor.  The pointer remains valid until
    // the next reset/begin and is suitable for tud_descriptor_string_cb().
    const uint8_t *stringDescriptor(uint8_t index, uint16_t langid,
                                    uint16_t *length) const;
    const CachedControlInfo *cachedControl(
        uint8_t bm_request_type, uint8_t b_request, uint16_t w_value,
        uint16_t w_index) const;
    uint8_t cachedControlCount() const;

private:
    static constexpr size_t kDeviceSize = 18;
    static constexpr size_t kEndpointLimit = 6;
    static constexpr size_t kHidSlotLimit = 8;
    static constexpr size_t kStringSlotLimit =
        makcm::bridge::kMaxStringDescriptors;
    static constexpr size_t kControlCacheLimit =
        makcm::bridge::kMaxCachedControls;

    struct StringSlot {
        bool used = false;
        uint8_t index = 0;
        uint16_t langid = 0;
        uint16_t length = 0;
        alignas(4) std::array<uint8_t,
                               makcm::bridge::kMaxStringDescriptorSize> data{};
        std::array<uint8_t, makcm::bridge::kMaxStringDescriptorSize / 8 + 1>
            received{};
        uint16_t received_count = 0;
    };

    struct ControlCacheSlot {
        CachedControlInfo info{};
        alignas(4) std::array<uint8_t,
                               makcm::bridge::kMaxCachedControlSize> data{};
        std::array<uint8_t,
                   (makcm::bridge::kMaxCachedControlSize + 7) / 8>
            received{};
    };

    struct BlobSlot {
        ProfileBlobInfo info{};
        makcm::bridge::ProfileBlobChunkHeader header{};
        alignas(4) std::array<uint8_t,
                               makcm::bridge::kMaxProfileBlobSize> data{};
        std::array<uint8_t,
                   (makcm::bridge::kMaxProfileBlobSize + 7) / 8>
            received{};
        uint16_t received_count = 0;
    };

    template <size_t N>
    static bool writeChunk(std::array<uint8_t, N> &storage,
                           std::array<uint8_t, (N + 7) / 8> &bitmap,
                           uint16_t &received_count, uint16_t offset,
                           const uint8_t *data, uint16_t length,
                           uint16_t limit);

    static void setError(ProfileValidation &result, uint8_t code,
                         const char *message);
    bool parseConfiguration(ProfileValidation &result);
    bool parseHidDescriptors(ProfileValidation &result);
    bool validateStrings(ProfileValidation &result) const;
    bool validateControlCache(ProfileValidation &result) const;
    bool validateProfileBlobs(ProfileValidation &result) const;
    bool isDescriptorComplete(const uint8_t *data, uint16_t length,
                              uint16_t received, size_t storage_size) const;

    makcm::bridge::DeviceProfile profile_{};
    std::array<uint8_t, kDeviceSize> device_{};
    std::array<uint8_t, makcm::bridge::kMaxConfigurationDescriptorSize>
        configuration_{};
    std::array<uint8_t, (kDeviceSize + 7) / 8> device_received_{};
    std::array<uint8_t,
               (makcm::bridge::kMaxConfigurationDescriptorSize + 7) / 8>
        configuration_received_{};
    uint16_t device_received_count_ = 0;
    uint16_t configuration_received_count_ = 0;
    std::array<std::array<uint8_t, makcm::bridge::kMaxReportDescriptorSize>,
               kHidSlotLimit>
        hid_data_{};
    std::array<std::array<uint8_t,
                          (makcm::bridge::kMaxReportDescriptorSize + 7) / 8>,
               kHidSlotLimit>
        hid_received_{};
    std::array<uint16_t, kHidSlotLimit> hid_received_count_{};
    std::array<HidReportInfo, kHidSlotLimit> hid_reports_{};
    std::array<StringSlot, kStringSlotLimit> strings_{};
    std::array<ControlCacheSlot, kControlCacheLimit> control_cache_{};
    std::array<BlobSlot, makcm::bridge::kMaxProfileBlobs> blobs_{};
    std::array<EndpointInfo, kEndpointLimit> endpoints_{};
    uint16_t device_length_ = 0;
    uint16_t configuration_length_ = 0;
    uint8_t endpoint_count_ = 0;
    uint8_t hid_count_ = 0;
    uint8_t blob_count_ = 0;
    bool begun_ = false;
    bool validated_ = false;
    bool capability_result_received_ = false;
    makcm::bridge::CapabilityResult capability_result_{};
    ProfileValidation validation_{};
};

} // namespace makcm::native
