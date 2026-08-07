#include "profile_store.h"
#include "ProxyCapabilityPlanner.h"
#include "HidMouseReport.h"

#include <algorithm>
#include <cstring>
#include <cstdio>

namespace makcm::native {
namespace {

constexpr uint8_t kDescriptorDevice = 0x01;
constexpr uint8_t kDescriptorConfiguration = 0x02;
constexpr uint8_t kDescriptorString = 0x03;
constexpr uint8_t kDescriptorInterface = 0x04;
constexpr uint8_t kDescriptorEndpoint = 0x05;
constexpr uint8_t kDescriptorHid = 0x21;
constexpr uint8_t kDescriptorReport = 0x22;

uint16_t le16(const uint8_t *p) {
    return static_cast<uint16_t>(p[0]) |
           (static_cast<uint16_t>(p[1]) << 8);
}

bool isBitSet(const uint8_t *bitmap, size_t index) {
    return (bitmap[index >> 3] & (1u << (index & 7))) != 0;
}

void setBit(uint8_t *bitmap, size_t index) {
    bitmap[index >> 3] = static_cast<uint8_t>(
        bitmap[index >> 3] | static_cast<uint8_t>(1u << (index & 7)));
}

} // namespace

ProfileStore::ProfileStore() { reset(); }

void ProfileStore::reset() {
    profile_ = {};
    device_.fill(0);
    configuration_.fill(0);
    device_received_.fill(0);
    configuration_received_.fill(0);
    for (auto &slot : hid_data_) slot.fill(0);
    for (auto &bitmap : hid_received_) bitmap.fill(0);
    hid_received_count_.fill(0);
    for (auto &info : hid_reports_) info = {};
    for (auto &slot : strings_) slot = {};
    for (auto &slot : control_cache_) {
        slot.info = {};
        slot.data.fill(0);
        slot.received.fill(0);
    }
    for (auto &slot : blobs_) {
        slot.info = {};
        slot.header = {};
        slot.data.fill(0);
        slot.received.fill(0);
        slot.received_count = 0;
    }
    for (auto &endpoint : endpoints_) endpoint = {};
    device_length_ = 0;
    configuration_length_ = 0;
    device_received_count_ = 0;
    configuration_received_count_ = 0;
    endpoint_count_ = 0;
    hid_count_ = 0;
    blob_count_ = 0;
    begun_ = false;
    validated_ = false;
    capability_result_received_ = false;
    capability_result_ = {};
    validation_ = {};
}

bool ProfileStore::begin(const makcm::bridge::DeviceProfile &profile) {
    reset();
    if (profile.version != makcm::bridge::kProtocolVersion ||
        profile.deviceDescriptorLength != kDeviceSize ||
        profile.configurationDescriptorLength == 0 ||
        profile.configurationDescriptorLength > configuration_.size()) {
        return false;
    }
    profile_ = profile;
    device_length_ = kDeviceSize;
    configuration_length_ = profile.configurationDescriptorLength;
    begun_ = true;
    return true;
}

template <size_t N>
bool ProfileStore::writeChunk(
    std::array<uint8_t, N> &storage,
    std::array<uint8_t, (N + 7) / 8> &bitmap, uint16_t &received_count,
    uint16_t offset, const uint8_t *data, uint16_t length, uint16_t limit) {
    if (data == nullptr && length != 0) return false;
    if (offset > limit || length > static_cast<uint16_t>(limit - offset) ||
        static_cast<size_t>(offset) + length > N) {
        return false;
    }
    for (uint16_t i = 0; i < length; ++i) {
        const size_t index = static_cast<size_t>(offset) + i;
        if (isBitSet(bitmap.data(), index) && storage[index] != data[i]) {
            return false;
        }
    }
    for (uint16_t i = 0; i < length; ++i) {
        const size_t index = static_cast<size_t>(offset) + i;
        if (!isBitSet(bitmap.data(), index)) {
            setBit(bitmap.data(), index);
            ++received_count;
        }
        storage[index] = data[i];
    }
    return true;
}

bool ProfileStore::appendDeviceDescriptor(uint16_t offset, const uint8_t *data,
                                          uint16_t length) {
    if (!begun_) return false;
    validated_ = false;
    return writeChunk(device_, device_received_, device_received_count_, offset,
                      data, length, kDeviceSize);
}

bool ProfileStore::appendConfigurationDescriptor(uint16_t offset,
                                                 const uint8_t *data,
                                                 uint16_t length) {
    if (!begun_) return false;
    validated_ = false;
    return writeChunk(configuration_, configuration_received_,
                      configuration_received_count_, offset, data, length,
                      configuration_length_);
}

bool ProfileStore::appendHidReportDescriptor(uint8_t interface_number,
                                             uint16_t offset,
                                             const uint8_t *data,
                                             uint16_t length) {
    if (!begun_ || interface_number >= 0xF0 ||
        static_cast<size_t>(offset) + length >
            makcm::bridge::kMaxReportDescriptorSize) {
        return false;
    }
    size_t slot = kHidSlotLimit;
    for (size_t i = 0; i < hid_count_; ++i) {
        if (hid_reports_[i].interface_number == interface_number) {
            slot = i;
            break;
        }
    }
    if (slot == kHidSlotLimit) {
        if (hid_count_ >= kHidSlotLimit) return false;
        slot = hid_count_++;
        hid_reports_[slot].interface_number = interface_number;
        hid_reports_[slot].data = hid_data_[slot].data();
    }
    validated_ = false;
    const bool ok = writeChunk(hid_data_[slot], hid_received_[slot],
                               hid_received_count_[slot], offset, data, length,
                               makcm::bridge::kMaxReportDescriptorSize);
    hid_reports_[slot].received_length = hid_received_count_[slot];
    return ok;
}

bool ProfileStore::appendStringDescriptor(uint8_t index, uint16_t langid,
                                          uint16_t offset, const uint8_t *data,
                                          uint16_t length) {
    if (!begun_ || (data == nullptr && length != 0) ||
        static_cast<size_t>(offset) + length >
            makcm::bridge::kMaxStringDescriptorSize) {
        return false;
    }
    StringSlot *slot = nullptr;
    for (auto &candidate : strings_) {
        if (candidate.used && candidate.index == index &&
            candidate.langid == langid) {
            slot = &candidate;
            break;
        }
    }
    if (slot == nullptr) {
        for (auto &candidate : strings_) {
            if (!candidate.used) {
                candidate.used = true;
                candidate.index = index;
                candidate.langid = langid;
                slot = &candidate;
                break;
            }
        }
    }
    if (slot == nullptr) return false;
    validated_ = false;
    if (!writeChunk(slot->data, slot->received, slot->received_count, offset,
                    data, length, makcm::bridge::kMaxStringDescriptorSize)) {
        return false;
    }
    if (offset == 0 && length >= 2) {
        const uint8_t declared = data[0];
        if (declared < 2 || declared > slot->data.size() ||
            data[1] != kDescriptorString) {
            return false;
        }
        slot->length = declared;
    }
    return true;
}

bool ProfileStore::appendProfileBlob(
    const makcm::bridge::ProfileBlobChunkHeader &header,
    const uint8_t *data, uint16_t length) {
    if (!begun_ || (data == nullptr && length != 0) ||
        header.totalLength == 0 ||
        header.totalLength > makcm::bridge::kMaxProfileBlobSize ||
        header.offset > header.totalLength ||
        length > static_cast<uint16_t>(header.totalLength - header.offset)) {
        return false;
    }

    // Keep the typed views used by TinyUSB in lockstep with the generic blob
    // store.  The generic store remains authoritative for integrity and for
    // descriptor kinds the parser does not understand.
    bool typed_ok = true;
    switch (header.type) {
    case makcm::bridge::ProfileBlobType::Device:
        typed_ok = header.totalLength == kDeviceSize &&
                   appendDeviceDescriptor(header.offset, data, length);
        break;
    case makcm::bridge::ProfileBlobType::Configuration:
        typed_ok = appendConfigurationDescriptor(header.offset, data, length);
        break;
    case makcm::bridge::ProfileBlobType::HidReport:
        typed_ok = header.key < 0xF0 && appendHidReportDescriptor(
            static_cast<uint8_t>(header.key), header.offset, data, length);
        break;
    case makcm::bridge::ProfileBlobType::String:
        typed_ok = header.key <= 0xFF && appendStringDescriptor(
            static_cast<uint8_t>(header.key), header.langid, header.offset,
            data, length);
        break;
    case makcm::bridge::ProfileBlobType::Bos:
    case makcm::bridge::ProfileBlobType::DeviceQualifier:
    case makcm::bridge::ProfileBlobType::Unknown:
        break;
    }
    if (!typed_ok) return false;

    BlobSlot *slot = nullptr;
    for (auto &candidate : blobs_) {
        if (candidate.info.used && candidate.info.type == header.type &&
            candidate.info.key == header.key &&
            candidate.info.langid == header.langid) {
            slot = &candidate;
            break;
        }
    }
    if (slot == nullptr) {
        for (auto &candidate : blobs_) {
            if (!candidate.info.used) {
                candidate.info.used = true;
                candidate.info.type = header.type;
                candidate.info.key = header.key;
                candidate.info.langid = header.langid;
                candidate.info.length = header.totalLength;
                candidate.info.crc = header.totalCrc;
                candidate.info.data = candidate.data.data();
                candidate.header = header;
                candidate.received_count = 0;
                slot = &candidate;
                ++blob_count_;
                break;
            }
        }
    }
    if (slot == nullptr || slot->info.length != header.totalLength ||
        slot->info.crc != header.totalCrc) {
        return false;
    }
    validated_ = false;
    return writeChunk(slot->data, slot->received, slot->received_count,
                      header.offset, data, length, header.totalLength);
}

bool ProfileStore::appendControlCache(
    const makcm::bridge::ProfileControlCacheHeader &header,
    const uint8_t *data, uint16_t length) {
    if (!begun_ || (data == nullptr && length != 0) ||
        header.totalLength == 0 ||
        header.totalLength > makcm::bridge::kMaxCachedControlSize ||
        header.offset > header.totalLength ||
        length > static_cast<uint16_t>(header.totalLength - header.offset)) {
        return false;
    }
    ControlCacheSlot *slot = nullptr;
    for (auto &candidate : control_cache_) {
        if (candidate.info.used &&
            candidate.info.bm_request_type == header.bmRequestType &&
            candidate.info.b_request == header.bRequest &&
            candidate.info.w_value == header.wValue &&
            candidate.info.w_index == header.wIndex) {
            slot = &candidate;
            break;
        }
    }
    if (slot == nullptr) {
        for (auto &candidate : control_cache_) {
            if (!candidate.info.used) {
                candidate.info.used = true;
                candidate.info.bm_request_type = header.bmRequestType;
                candidate.info.b_request = header.bRequest;
                candidate.info.w_value = header.wValue;
                candidate.info.w_index = header.wIndex;
                candidate.info.w_length = header.wLength;
                candidate.info.received_length = 0;
                candidate.info.data = candidate.data.data();
                slot = &candidate;
                break;
            }
        }
    }
    if (slot == nullptr || slot->info.w_length != header.wLength) return false;
    for (uint16_t i = 0; i < length; ++i) {
        const size_t index = static_cast<size_t>(header.offset) + i;
        if (isBitSet(slot->received.data(), index) && slot->data[index] != data[i])
            return false;
    }
    for (uint16_t i = 0; i < length; ++i) {
        const size_t index = static_cast<size_t>(header.offset) + i;
        if (!isBitSet(slot->received.data(), index)) {
            setBit(slot->received.data(), index);
            ++slot->info.received_length;
        }
        slot->data[index] = data[i];
    }
    validated_ = false;
    return true;
}

void ProfileStore::setError(ProfileValidation &result, uint8_t code,
                            const char *message) {
    result.ok = false;
    result.error_code = code;
    std::snprintf(result.message, sizeof(result.message), "%s", message);
}

bool ProfileStore::isDescriptorComplete(const uint8_t *data, uint16_t length,
                                        uint16_t received,
                                        size_t storage_size) const {
    return data != nullptr && length >= 2 && length <= storage_size &&
           received >= length;
}

bool ProfileStore::parseConfiguration(ProfileValidation &result) {
    if (configuration_received_count_ < 9 || configuration_length_ < 9) {
        setError(result, 20, "configuration descriptor incomplete");
        return false;
    }
    const uint8_t *config = configuration_.data();
    if (config[0] != 9 || config[1] != kDescriptorConfiguration) {
        setError(result, 21, "bad configuration header");
        return false;
    }
    const uint16_t total = le16(config + 2);
    if (total != configuration_length_ || total < 9 ||
        total > configuration_.size() ||
        config[5] == 0 || config[4] == 0) {
        setError(result, 22, "configuration length/value mismatch");
        return false;
    }
    if ((config[7] & 0x80) == 0 || config[8] > 250) {
        setError(result, 23, "invalid configuration power flags");
        return false;
    }
    if (profile_.configAttributes != 0 &&
        profile_.configAttributes != config[7]) {
        setError(result, 24, "profile configuration attributes differ");
        return false;
    }

    struct InterfaceState {
        bool used = false;
        uint8_t number = 0;
        uint8_t expected_endpoints = 0;
        uint8_t actual_endpoints = 0;
        uint8_t alternate = 0;
    } interfaces[16]{};
    bool endpoint_seen[256]{};
    uint8_t current_interface = 0xFF;
    uint8_t current_alt = 0xFF;
    endpoint_count_ = 0;
    endpoints_.fill({});
    result.interface_count = 0;
    result.endpoint_count = 0;
    result.in_count = 0;
    result.out_count = 0;
    result.max_packet = 0;

    size_t offset = 0;
    while (offset < total) {
        if (offset + 2 > total) {
            setError(result, 25, "descriptor truncated");
            return false;
        }
        const uint8_t length = config[offset];
        const uint8_t type = config[offset + 1];
        if (length < 2 || offset + length > total) {
            setError(result, 26, "descriptor length invalid");
            return false;
        }
        const uint8_t *desc = config + offset;
        if (type == kDescriptorInterface) {
            if (length < 9) {
                setError(result, 27, "interface descriptor too short");
                return false;
            }
            current_interface = desc[2];
            current_alt = desc[3];
            if (current_alt != 0) {
                setError(result, 28, "alternate interface unsupported");
                return false;
            }
            size_t slot = 16;
            bool already_seen = false;
            for (size_t i = 0; i < 16; ++i) {
                if (interfaces[i].used && interfaces[i].number == current_interface) {
                    slot = i;
                    already_seen = true;
                    break;
                }
            }
            if (already_seen) {
                setError(result, 28, "duplicate alternate-zero interface");
                return false;
            }
            if (slot == 16) {
                for (size_t i = 0; i < 16; ++i) {
                    if (!interfaces[i].used) {
                        slot = i;
                        interfaces[i].used = true;
                        interfaces[i].number = current_interface;
                        interfaces[i].expected_endpoints = desc[4];
                        interfaces[i].alternate = current_alt;
                        break;
                    }
                }
            }
            if (slot == 16 || desc[4] > 6) {
                setError(result, 29, "too many interfaces/endpoints");
                return false;
            }
            // A repeated interface number is only valid as the same alt
            // setting, which was already rejected above.
            ++result.interface_count;
        } else if (type == kDescriptorEndpoint) {
            if (length < 7 || current_interface == 0xFF || current_alt != 0) {
                setError(result, 30, "endpoint without interface");
                return false;
            }
            const uint8_t address = desc[2];
            const uint8_t number = address & 0x0F;
            const uint16_t raw_mps = le16(desc + 4);
            const uint16_t mps = raw_mps & 0x07FF;
            const uint8_t transfer_type = desc[3] & 0x03;
            if (number == 0 || number > 6 || endpoint_seen[address] ||
                (raw_mps & 0x1800) != 0 || mps == 0 || mps > 64 ||
                transfer_type == 0 || transfer_type == 1) {
                setError(result, 31, "endpoint resource or type unsupported");
                return false;
            }
            if (endpoint_count_ >= endpoints_.size()) {
                setError(result, 32, "too many non-control endpoints");
                return false;
            }
            endpoint_seen[address] = true;
            EndpointInfo &endpoint = endpoints_[endpoint_count_++];
            endpoint.address = address;
            endpoint.interface_number = current_interface;
            endpoint.attributes = desc[3];
            endpoint.max_packet = mps;
            endpoint.interval = desc[6];
            ++result.endpoint_count;
            result.max_packet = std::max(result.max_packet, mps);
            if ((address & 0x80) != 0) {
                ++result.in_count;
            } else {
                ++result.out_count;
            }
            for (auto &state : interfaces) {
                if (state.used && state.number == current_interface) {
                    ++state.actual_endpoints;
                    break;
                }
            }
        }
        offset += length;
    }
    for (const auto &state : interfaces) {
        if (state.used && state.expected_endpoints != state.actual_endpoints) {
            setError(result, 33, "interface endpoint count mismatch");
            return false;
        }
    }
    if (result.interface_count != config[4] || result.interface_count == 0 ||
        result.in_count > 4 || result.out_count > 4 ||
        result.in_count + result.out_count > 6) {
        setError(result, 34, "DWC2 endpoint budget exceeded");
        return false;
    }
    if (profile_.endpointCount != 0 &&
        profile_.endpointCount != result.endpoint_count) {
        setError(result, 34, "profile endpoint count differs");
        return false;
    }
    return true;
}

bool ProfileStore::parseHidDescriptors(ProfileValidation &result) {
    // The HID descriptor is in the configuration stream, while the report
    // descriptor arrives on its own protocol-v5 chunks.  Validate each HID
    // interface's expected length and coverage after the topology pass.
    size_t offset = 0;
    uint8_t current_interface = 0xFF;
    uint8_t current_interface_class = 0xFF;
    while (offset < configuration_length_) {
        const uint8_t length = configuration_[offset];
        const uint8_t type = configuration_[offset + 1];
        if (length < 2 || offset + length > configuration_length_) {
            setError(result, 35, "HID descriptor stream malformed");
            return false;
        }
        if (type == kDescriptorInterface && length >= 9) {
            current_interface = configuration_[offset + 2];
            current_interface_class = configuration_[offset + 5];
        } else if (type == kDescriptorHid && current_interface != 0xFF &&
                   current_interface_class == 0x03) {
            // 0x21 is also used by vendor/XInput interfaces.  Only a HID
            // class interface owns the HID descriptor grammar and its 0x22
            // report-length children; vendor 0x21 blocks must be preserved
            // raw without making ProfileStore reject the whole profile.
            if (length < 9 || configuration_[offset + 3] == 0) {
                setError(result, 36, "HID descriptor malformed");
                return false;
            }
            uint16_t report_length = 0;
            size_t child = offset + 6;
            for (uint8_t i = 0; i < configuration_[offset + 5] &&
                               child + 3 <= offset + length;
                 ++i, child += 3) {
                // The HID descriptor's subordinate entries are three bytes:
                // bDescriptorType followed by the little-endian wLength.
                if (configuration_[child] == kDescriptorReport) {
                    report_length = le16(configuration_.data() + child + 1);
                    break;
                }
            }
            if (report_length == 0 || report_length >
                    makcm::bridge::kMaxReportDescriptorSize) {
                setError(result, 37, "HID report length unsupported");
                return false;
            }
            HidReportInfo *found = nullptr;
            for (uint8_t i = 0; i < hid_count_; ++i) {
                if (hid_reports_[i].interface_number == current_interface) {
                    found = &hid_reports_[i];
                    break;
                }
            }
            if (found == nullptr || found->received_length < report_length) {
                setError(result, 38, "HID report descriptor incomplete");
                return false;
            }
            found->expected_length = report_length;
            // The generic v5 Blob table is authoritative.  Older right-side
            // firmware used `reportDescriptorLength` as a cumulative legacy
            // observer field for composite devices, so comparing that field
            // to one interface would reject otherwise complete keyboard/mouse
            // profiles.  Per-interface report lengths are already checked by
            // this descriptor/blob pair above; do not gate validation on the
            // legacy aggregate.
        }
        offset += length;
    }
    return true;
}

bool ProfileStore::validateStrings(ProfileValidation &result) const {
    for (const auto &slot : strings_) {
        if (!slot.used) continue;
        if (slot.length < 2 || slot.length > slot.data.size() ||
            slot.data[0] != slot.length || slot.data[1] != kDescriptorString ||
            slot.received_count < slot.length) {
            setError(result, 40, "string descriptor incomplete");
            return false;
        }
        if ((slot.length & 1u) != 0) {
            setError(result, 41, "UTF-16 string length is odd");
            return false;
        }
    }
    return true;
}

bool ProfileStore::validateControlCache(ProfileValidation &result) const {
    for (const auto &slot : control_cache_) {
        if (!slot.info.used) continue;
        if (slot.info.w_length == 0 ||
            slot.info.received_length != slot.info.w_length ||
            slot.info.received_length > makcm::bridge::kMaxCachedControlSize) {
            setError(result, 42, "cached control response incomplete");
            return false;
        }
        if ((slot.info.bm_request_type & 0x80) == 0) {
            setError(result, 43, "cached control response is not IN");
            return false;
        }
    }
    return true;
}

bool ProfileStore::validateProfileBlobs(ProfileValidation &result) const {
    if (profile_.blobCount != 0 && profile_.blobCount != blob_count_) {
        setError(result, 45, "profile blob count mismatch");
        return false;
    }
    uint32_t aggregate_crc = 0;
    for (const auto &slot : blobs_) {
        if (!slot.info.used) continue;
        if (slot.info.length == 0 ||
            slot.received_count < slot.info.length) {
            setError(result, 46, "profile blob incomplete");
            return false;
        }
        if (makcm::bridge::crc16Ccitt(slot.data.data(), slot.info.length) !=
            slot.info.crc) {
            setError(result, 47, "profile blob CRC mismatch");
            return false;
        }
        aggregate_crc ^= slot.info.crc;
        if (slot.info.type == makcm::bridge::ProfileBlobType::Bos &&
            (slot.info.length < 5 || slot.data[0] != 5 ||
             slot.data[1] != 0x0F || le16(slot.data.data() + 2) !=
                 slot.info.length)) {
            setError(result, 48, "BOS blob malformed");
            return false;
        }
        if (slot.info.type ==
                makcm::bridge::ProfileBlobType::DeviceQualifier &&
            (slot.info.length != 10 || slot.data[0] != 10 ||
             slot.data[1] != 0x06)) {
            setError(result, 49, "qualifier blob malformed");
            return false;
        }
    }
    if (profile_.descriptorCrc != 0 && profile_.descriptorCrc != aggregate_crc) {
        setError(result, 51, "profile descriptor CRC mismatch");
        return false;
    }
    result.blob_count = blob_count_;
    return true;
}

ProfileValidation ProfileStore::validate() {
    validation_ = {};
    validation_.ok = false;
    if (!begun_) {
        setError(validation_, 1, "profile not begun");
        return validation_;
    }
    validation_.capability = makcm::bridge::ProxyCapability::Unsupported;
    validation_.capability_reason = 0;
    if (!capability_result_received_) {
        setError(validation_, 52, "capability result missing");
        return validation_;
    }
    if (capability_result_received_ && capability_result_.capability ==
            makcm::bridge::ProxyCapability::Unsupported) {
        validation_.capability_reason = static_cast<uint8_t>(
            capability_result_.reason);
        setError(validation_, 50, "right host rejected proxy capability");
        return validation_;
    }
    if (!isDescriptorComplete(device_.data(), device_length_,
                              device_received_count_, device_.size()) ||
        device_[0] != 18 || device_[1] != kDescriptorDevice) {
        setError(validation_, 10, "device descriptor incomplete");
        return validation_;
    }
    const uint8_t device_class = device_[4];
    if (profile_.vid != le16(device_.data() + 8) ||
        profile_.pid != le16(device_.data() + 10) ||
        profile_.bcdUsb != le16(device_.data() + 2) ||
        profile_.deviceClass != device_class) {
        setError(validation_, 11, "profile metadata differs from device descriptor");
        return validation_;
    }
    if (!isDescriptorComplete(configuration_.data(), configuration_length_,
                              configuration_received_count_,
                              configuration_.size())) {
        setError(validation_, 12, "configuration descriptor incomplete");
        return validation_;
    }
    const makcm::bridge::CapabilityInput capability_input{
        device_.data(), device_length_, configuration_.data(),
        configuration_length_, profile_.usbSpeed, 4000000};
    const makcm::bridge::CapabilityResult planned =
        makcm::bridge::planCapability(capability_input);
    validation_.capability = planned.capability;
    validation_.capability_reason = static_cast<uint8_t>(planned.reason);
    validation_.interface_count = planned.interfaceCount;
    validation_.endpoint_count = planned.endpointCount;
    validation_.in_count = planned.inCount;
    validation_.out_count = planned.outCount;
    validation_.max_endpoint_number = planned.maxEndpointNumber;
    validation_.max_packet = planned.maxPacketSize;
    validation_.estimated_bits_per_second = planned.estimatedBitsPerSecond;
    if (planned.capability == makcm::bridge::ProxyCapability::Unsupported) {
        char message[96]{};
        std::snprintf(message, sizeof(message), "unsupported: %s",
                      makcm::bridge::capabilityReasonText(planned.reason));
        setError(validation_, 44, message);
        return validation_;
    }
    if (capability_result_.capability != planned.capability ||
        capability_result_.reason != planned.reason ||
        capability_result_.speed != planned.speed ||
        capability_result_.configurationCount != planned.configurationCount ||
        capability_result_.interfaceCount != planned.interfaceCount ||
        capability_result_.endpointCount != planned.endpointCount ||
        capability_result_.inCount != planned.inCount ||
        capability_result_.outCount != planned.outCount ||
        capability_result_.maxEndpointNumber != planned.maxEndpointNumber ||
        capability_result_.maxPacketSize != planned.maxPacketSize ||
        capability_result_.estimatedBitsPerSecond !=
            planned.estimatedBitsPerSecond) {
        setError(validation_, 53, "capability result differs from local plan");
        return validation_;
    }
    if (!parseConfiguration(validation_) ||
        !parseHidDescriptors(validation_) || !validateStrings(validation_) ||
        !validateControlCache(validation_) ||
        !validateProfileBlobs(validation_)) {
        return validation_;
    }
    validation_.ok = true;
    validation_.error_code = 0;
    std::snprintf(validation_.message, sizeof(validation_.message), "ok");
    validated_ = true;
    return validation_;
}

const EndpointInfo *ProfileStore::endpoint(uint8_t address) const {
    for (uint8_t i = 0; i < endpoint_count_; ++i) {
        if (endpoints_[i].address == address) return &endpoints_[i];
    }
    return nullptr;
}

const EndpointInfo *ProfileStore::mouseEndpoint() const {
    for (uint8_t i = 0; i < endpoint_count_; ++i) {
        const auto &ep = endpoints_[i];
        if ((ep.address & 0x80) != 0) {
            // Prefer the report grammar over the Boot subclass/protocol.  A
            // large number of gaming keyboards/mice expose a perfectly valid
            // HID Mouse application collection with subclass/protocol 0, so
            // checking only (3, 1, 2) silently disables descriptor-derived
            // mouse injection for those devices.
            const HidReportInfo *report = hidReport(ep.interface_number);
            if (report != nullptr && report->data != nullptr &&
                report->received_length != 0) {
                makcm::hid::MouseReportLayout layout;
                if (makcm::hid::parseMouseReportDescriptor(
                        report->data,
                        report->expected_length != 0
                            ? report->expected_length
                            : report->received_length,
                        layout)) {
                    return &ep;
                }
            }

            // Keep the Boot Mouse fallback for a profile whose report blob is
            // unavailable during legacy/rolling handshakes.  The generic
            // transport still forwards all endpoints even when this policy
            // probe cannot identify a mouse collection.
            size_t offset = 0;
            uint8_t current = 0xFF;
            while (offset + 9 <= configuration_length_) {
                const uint8_t length = configuration_[offset];
                if (length < 2 || offset + length > configuration_length_) break;
                if (configuration_[offset + 1] == kDescriptorInterface &&
                    length >= 9) {
                    current = configuration_[offset + 2];
                    if (current == ep.interface_number &&
                        configuration_[offset + 5] == 3 &&
                        configuration_[offset + 6] == 1 &&
                        configuration_[offset + 7] == 2) {
                        return &ep;
                    }
                }
                offset += length;
            }
        }
    }
    return nullptr;
}

const EndpointInfo *ProfileStore::xinputEndpoint() const {
    if (profile_.inEndpoint != 0) return endpoint(profile_.inEndpoint);
    for (uint8_t i = 0; i < endpoint_count_; ++i) {
        if ((endpoints_[i].address & 0x80) != 0) return &endpoints_[i];
    }
    return nullptr;
}

const HidReportInfo *ProfileStore::hidReport(uint8_t interface_number) const {
    for (uint8_t i = 0; i < hid_count_; ++i) {
        if (hid_reports_[i].interface_number == interface_number) {
            return &hid_reports_[i];
        }
    }
    return nullptr;
}

const uint8_t *ProfileStore::stringDescriptor(uint8_t index, uint16_t langid,
                                               uint16_t *length) const {
    if (length != nullptr) *length = 0;
    for (const auto &slot : strings_) {
        if (slot.used && slot.index == index &&
            (slot.langid == langid || langid == 0 || slot.langid == 0)) {
            if (length != nullptr) *length = slot.length;
            return slot.data.data();
        }
    }
    return nullptr;
}

const CachedControlInfo *ProfileStore::cachedControl(
    uint8_t bm_request_type, uint8_t b_request, uint16_t w_value,
    uint16_t w_index) const {
    for (const auto &slot : control_cache_) {
        if (slot.info.used &&
            slot.info.bm_request_type == bm_request_type &&
            slot.info.b_request == b_request && slot.info.w_value == w_value &&
            slot.info.w_index == w_index &&
            slot.info.received_length == slot.info.w_length) {
            return &slot.info;
        }
    }
    return nullptr;
}

uint8_t ProfileStore::cachedControlCount() const {
    uint8_t count = 0;
    for (const auto &slot : control_cache_) {
        if (slot.info.used && slot.info.received_length == slot.info.w_length)
            ++count;
    }
    return count;
}

const ProfileBlobInfo *ProfileStore::blob(uint8_t index) const {
    if (index >= blobs_.size() || !blobs_[index].info.used) return nullptr;
    return &blobs_[index].info;
}

const uint8_t *ProfileStore::bosDescriptor() const {
    for (const auto &slot : blobs_) {
        if (slot.info.used &&
            slot.info.type == makcm::bridge::ProfileBlobType::Bos)
            return slot.data.data();
    }
    return nullptr;
}

uint16_t ProfileStore::bosDescriptorLength() const {
    for (const auto &slot : blobs_) {
        if (slot.info.used &&
            slot.info.type == makcm::bridge::ProfileBlobType::Bos)
            return slot.info.length;
    }
    return 0;
}

const uint8_t *ProfileStore::qualifierDescriptor() const {
    for (const auto &slot : blobs_) {
        if (slot.info.used && slot.info.type ==
                makcm::bridge::ProfileBlobType::DeviceQualifier)
            return slot.data.data();
    }
    return nullptr;
}

uint16_t ProfileStore::qualifierDescriptorLength() const {
    for (const auto &slot : blobs_) {
        if (slot.info.used && slot.info.type ==
                makcm::bridge::ProfileBlobType::DeviceQualifier)
            return slot.info.length;
    }
    return 0;
}

// Explicit instantiations keep the template implementation in this
// translation unit and avoid pulling it into every user of ProfileStore.
template bool ProfileStore::writeChunk<18>(
    std::array<uint8_t, 18> &, std::array<uint8_t, 3> &, uint16_t &, uint16_t,
    const uint8_t *, uint16_t, uint16_t);
template bool ProfileStore::writeChunk<
    makcm::bridge::kMaxConfigurationDescriptorSize>(
    std::array<uint8_t, makcm::bridge::kMaxConfigurationDescriptorSize> &,
    std::array<uint8_t,
               (makcm::bridge::kMaxConfigurationDescriptorSize + 7) / 8> &,
    uint16_t &, uint16_t, const uint8_t *, uint16_t, uint16_t);
template bool ProfileStore::writeChunk<
    makcm::bridge::kMaxReportDescriptorSize>(
    std::array<uint8_t, makcm::bridge::kMaxReportDescriptorSize> &,
    std::array<uint8_t,
               (makcm::bridge::kMaxReportDescriptorSize + 7) / 8> &,
    uint16_t &, uint16_t, const uint8_t *, uint16_t, uint16_t);
template bool ProfileStore::writeChunk<
    makcm::bridge::kMaxStringDescriptorSize>(
    std::array<uint8_t, makcm::bridge::kMaxStringDescriptorSize> &,
    std::array<uint8_t,
               (makcm::bridge::kMaxStringDescriptorSize + 7) / 8> &,
    uint16_t &, uint16_t, const uint8_t *, uint16_t, uint16_t);

} // namespace makcm::native
