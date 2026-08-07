#include "controller_profile_registry.h"

#include <array>
#include <cstring>

#include "controller_codec.h"
#include "endpoint_router.h"
#include "mbedtls/sha256.h"
#include "profile_store.h"

namespace makcm::native {
namespace {

struct GeneratedAxisSpec {
    uint16_t offset;
    int32_t center;
    int32_t minimum;
    int32_t maximum;
    int8_t sign;
};

struct GeneratedControllerProfileSpec {
    const char *format_id;
    const char *cal_id;
    uint16_t vid;
    uint16_t pid;
    uint16_t bcd_device;
    uint32_t descriptor_hash;
    uint32_t hid_report_hash;
    std::array<uint8_t, 32> descriptor_sha256;
    std::array<uint8_t, 32> hid_report_sha256;
    uint8_t interface_number;
    uint8_t endpoint_address;
    uint8_t report_id;
    uint16_t report_length;
    uint16_t format_version;
    uint16_t variant;
    makcm::stick::CodecFieldEncoding encoding;
    GeneratedAxisSpec x;
    GeneratedAxisSpec y;
    // Keep legacy generated HID entries source-compatible: entries emitted
    // before explicit no-HID support omit this trailing field and therefore
    // retain the historical HID-required behavior.  New vendor/XInput
    // entries must set it explicitly to false.
    bool hid_report_present = true;
};

#if (defined(MAKCM_TEST_CONTROLLER_PROFILES) && \
     __has_include(<controller_profiles.local.inc>)) || \
    (!defined(MAKCM_TEST_CONTROLLER_PROFILES) && \
     __has_include("controller_profiles.local.inc"))
using makcm::stick::CodecFieldEncoding;
const GeneratedControllerProfileSpec kGeneratedProfiles[] = {
#if defined(MAKCM_TEST_CONTROLLER_PROFILES)
#include <controller_profiles.local.inc>
#else
#include "controller_profiles.local.inc"
#endif
};
constexpr size_t kGeneratedProfileCount =
    sizeof(kGeneratedProfiles) / sizeof(kGeneratedProfiles[0]);
#else
const GeneratedControllerProfileSpec *const kGeneratedProfiles = nullptr;
constexpr size_t kGeneratedProfileCount = 0;
#endif

uint16_t readLe16(const uint8_t *data) {
    return static_cast<uint16_t>(data[0]) |
           (static_cast<uint16_t>(data[1]) << 8);
}

bool sha256Parts(const uint8_t *first, size_t first_length,
                 const uint8_t *second, size_t second_length,
                 std::array<uint8_t, 32> &digest) {
    mbedtls_sha256_context context;
    mbedtls_sha256_init(&context);
    const bool ok = mbedtls_sha256_starts(&context, 0) == 0 &&
                    mbedtls_sha256_update(&context, first, first_length) == 0 &&
                    (second_length == 0 ||
                     mbedtls_sha256_update(&context, second, second_length) == 0) &&
                    mbedtls_sha256_finish(&context, digest.data()) == 0;
    mbedtls_sha256_free(&context);
    return ok;
}

uint32_t compactHash(const std::array<uint8_t, 32> &digest) {
    return (static_cast<uint32_t>(digest[0]) << 24) |
           (static_cast<uint32_t>(digest[1]) << 16) |
           (static_cast<uint32_t>(digest[2]) << 8) |
           static_cast<uint32_t>(digest[3]);
}

bool isZeroSha256(const std::array<uint8_t, 32> &digest) {
    for (const uint8_t byte : digest) {
        if (byte != 0) return false;
    }
    return true;
}

makcm::stick::StickAxisField axisField(
    const GeneratedAxisSpec &spec,
    makcm::stick::CodecFieldEncoding encoding) {
    makcm::stick::StickAxisField result;
    result.offset = spec.offset;
    result.encoding = encoding;
    result.center = spec.center;
    result.minimum = spec.minimum;
    result.maximum = spec.maximum;
    result.sign = spec.sign;
    return result;
}

} // namespace

size_t generatedControllerProfileCount() { return kGeneratedProfileCount; }

bool installGeneratedControllerProfile(EndpointRouter &router,
                                       const ProfileStore &profile) {
    if (!profile.ready() || profile.deviceDescriptorLength() != 18 ||
        profile.configurationDescriptor() == nullptr) return false;
    const uint8_t *device = profile.deviceDescriptor();
    const uint16_t vid = readLe16(device + 8);
    const uint16_t pid = readLe16(device + 10);
    const uint16_t bcd_device = readLe16(device + 12);

    std::array<uint8_t, 32> descriptor_sha{};
    if (!sha256Parts(device, profile.deviceDescriptorLength(),
                     profile.configurationDescriptor(),
                     profile.configurationDescriptorLength(), descriptor_sha)) {
        return false;
    }

    for (size_t index = 0; index < kGeneratedProfileCount; ++index) {
        const auto &spec = kGeneratedProfiles[index];
        if (spec.vid != vid || spec.pid != pid || spec.bcd_device != bcd_device ||
            spec.descriptor_hash != compactHash(descriptor_sha) ||
            spec.descriptor_sha256 != descriptor_sha) {
            continue;
        }
        const EndpointInfo *endpoint = profile.endpoint(spec.endpoint_address);
        if (endpoint == nullptr ||
            endpoint->interface_number != spec.interface_number) {
            continue;
        }
        std::array<uint8_t, 32> hid_sha{};
        if (spec.hid_report_present) {
            const HidReportInfo *hid = profile.hidReport(spec.interface_number);
            if (hid == nullptr || hid->data == nullptr ||
                hid->received_length == 0 ||
                !sha256Parts(hid->data, hid->received_length, nullptr, 0,
                             hid_sha) ||
                spec.hid_report_hash != compactHash(hid_sha) ||
                spec.hid_report_sha256 != hid_sha) {
                continue;
            }
        } else {
            // A no-HID entry is explicit and fail-closed.  Never let a
            // non-zero/garbage HID identity silently turn into a no-HID
            // match, and never borrow a report descriptor from another
            // interface in a composite device.
            if (spec.hid_report_hash != 0 ||
                !isZeroSha256(spec.hid_report_sha256) ||
                profile.hidReport(spec.interface_number) != nullptr) {
                continue;
            }
        }

        makcm::stick::ControllerProfileIdentity identity;
        identity.vid = vid;
        identity.pid = pid;
        identity.bcdDevice = bcd_device;
        identity.descriptorHash = compactHash(descriptor_sha);
        identity.hidReportHash = spec.hid_report_present ? compactHash(hid_sha) : 0;
        identity.descriptorSha256 = descriptor_sha;
        if (spec.hid_report_present) identity.hidReportSha256 = hid_sha;
        identity.interfaceNumber = spec.interface_number;
        identity.endpointAddress = spec.endpoint_address;
        identity.reportId = spec.report_id;
        identity.reportLength = spec.report_length;
        identity.formatVersion = spec.format_version;
        identity.variant = spec.variant;

        makcm::stick::ControllerProfile allowed;
        allowed.identity = identity;
        allowed.x = axisField(spec.x, spec.encoding);
        allowed.y = axisField(spec.y, spec.encoding);
        if (router.configureExactStickProfile(spec.format_id, spec.cal_id,
                                              allowed, identity)) {
            return true;
        }
    }
    return false;
}

} // namespace makcm::native
