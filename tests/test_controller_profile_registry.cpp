#include "controller_profile_registry.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>

#include "endpoint_router.h"
#include "mbedtls/sha256.h"
#include "profile_store.h"

namespace {

using makcm::native::EndpointRouter;
using makcm::native::ProfileStore;

ProfileStore profileWithEndpoint(uint8_t address, uint8_t interface_number,
                                 const uint8_t *configuration =
                                     kMakcmTestConfiguration.data(),
                                 uint16_t configuration_length =
                                     static_cast<uint16_t>(
                                         kMakcmTestConfiguration.size())) {
    ProfileStore profile;
    profile.setDescriptors(kMakcmTestDevice.data(),
                           static_cast<uint16_t>(kMakcmTestDevice.size()),
                           configuration, configuration_length);
    profile.setEndpoint(address, interface_number);
    return profile;
}

bool allZero(const std::array<uint8_t, 32> &bytes) {
    return std::all_of(bytes.begin(), bytes.end(),
                       [](uint8_t byte) { return byte == 0; });
}

void testValidNoHidUsesZeroObservedHash() {
    // A composite profile may expose HID reports on other interfaces.  The
    // no-HID entry must only inspect the explicitly selected target interface.
    ProfileStore profile = profileWithEndpoint(0x81, 0);
    profile.setHidReport(2, kMakcmTestHid.data(),
                         static_cast<uint16_t>(kMakcmTestHid.size()));
    EndpointRouter router;
    assert(makcm::native::installGeneratedControllerProfile(router, profile));
    assert(router.called);
    assert(router.accepted_identity.interfaceNumber == 0);
    assert(router.accepted_identity.endpointAddress == 0x81);
    assert(router.accepted_identity.hidReportHash == 0);
    assert(allZero(router.accepted_identity.hidReportSha256));
}

void testNoHidWithNonZeroSpecHashIsRejected() {
    ProfileStore profile = profileWithEndpoint(0x82, 1);
    EndpointRouter router;
    assert(!makcm::native::installGeneratedControllerProfile(router, profile));
    assert(!router.called);
}

void testHidRequiredWithoutTargetReportIsRejected() {
    ProfileStore profile = profileWithEndpoint(0x83, 2);
    EndpointRouter router;
    assert(!makcm::native::installGeneratedControllerProfile(router, profile));
    assert(!router.called);
}

void testHidRequiredExactShaStillMatches() {
    ProfileStore profile = profileWithEndpoint(0x83, 2);
    profile.setHidReport(2, kMakcmTestHid.data(),
                         static_cast<uint16_t>(kMakcmTestHid.size()));
    EndpointRouter router;
    assert(makcm::native::installGeneratedControllerProfile(router, profile));
    assert(router.called);
    assert(router.accepted_identity.hidReportHash == makcm_test_hid_hash());
    assert(router.accepted_identity.hidReportSha256 == makcm_test_hid_sha());
}

void testNoHidTargetReportIsRejected() {
    ProfileStore profile = profileWithEndpoint(0x81, 0);
    profile.setHidReport(0, nullptr, 0);
    EndpointRouter router;
    assert(!makcm::native::installGeneratedControllerProfile(router, profile));
    assert(!router.called);
}

void testWrongDescriptorAndEndpointAreRejected() {
    auto changed_configuration = kMakcmTestConfiguration;
    changed_configuration[3] ^= 0x01;
    ProfileStore wrong_descriptor = profileWithEndpoint(
        0x81, 0, changed_configuration.data(),
        static_cast<uint16_t>(changed_configuration.size()));
    EndpointRouter descriptor_router;
    assert(!makcm::native::installGeneratedControllerProfile(
        descriptor_router, wrong_descriptor));
    assert(!descriptor_router.called);

    ProfileStore wrong_endpoint = profileWithEndpoint(0x84, 0);
    EndpointRouter endpoint_router;
    assert(!makcm::native::installGeneratedControllerProfile(
        endpoint_router, wrong_endpoint));
    assert(!endpoint_router.called);
}

} // namespace

int main() {
    assert(makcm::native::generatedControllerProfileCount() == 3);
    testValidNoHidUsesZeroObservedHash();
    testNoHidWithNonZeroSpecHashIsRejected();
    testHidRequiredWithoutTargetReportIsRejected();
    testHidRequiredExactShaStillMatches();
    testNoHidTargetReportIsRejected();
    testWrongDescriptorAndEndpointAreRejected();
    return 0;
}
