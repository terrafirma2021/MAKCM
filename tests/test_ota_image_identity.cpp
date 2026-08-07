#include <cassert>
#include <cstring>

#include "OtaImageIdentity.h"

using namespace makcm::ota;

namespace {

EmbeddedImageIdentity makeIdentity() {
    EmbeddedImageIdentity identity{};
    std::memcpy(identity.magic, kEmbeddedIdentityMagic,
                sizeof(identity.magic));
    identity.version = kEmbeddedIdentityVersion;
    identity.size = sizeof(identity);
    identity.role = static_cast<uint8_t>(Target::Left);
    identity.generation = 14;
    identity.partition_schema = 0x4C010001u;
    identity.protocol_version = 1;
    std::memcpy(identity.hardware_revision, "makcm-s3-r1", 12);
    std::memcpy(identity.project_name, "makcm_left_idf", 15);
    std::memcpy(identity.app_version, "makcm-g14", 10);
    std::memcpy(identity.trailer, kEmbeddedIdentityTrailer,
                sizeof(identity.trailer));
    return identity;
}

} // namespace

int main() {
    static_assert(sizeof(EmbeddedImageIdentity) == 116,
                  "embedded identity wire layout changed");

    EmbeddedImageIdentity identity = makeIdentity();
    assert(validEmbeddedImageIdentity(identity));

    EmbeddedImageIdentity bad = identity;
    bad.reserved0[1] = 1;
    assert(!validEmbeddedImageIdentity(bad));

    bad = identity;
    bad.generation = 0;
    assert(!validEmbeddedImageIdentity(bad));

    bad = identity;
    std::memset(bad.project_name, 'x', sizeof(bad.project_name));
    assert(!validEmbeddedImageIdentity(bad));

    bad = identity;
    bad.app_version[11] = 'x';
    assert(!validEmbeddedImageIdentity(bad));

    bad = identity;
    bad.trailer[0] ^= 0x01;
    assert(!validEmbeddedImageIdentity(bad));

    return 0;
}
