#pragma once

#include <stddef.h>

namespace makcm::native {

class EndpointRouter;
class ProfileStore;

// Returns true only when a generated allow-list entry exactly matches the
// active Device+Configuration bundle, HID report descriptor, interface and
// endpoint. The repository default contains zero entries by design.
bool installGeneratedControllerProfile(EndpointRouter &router,
                                       const ProfileStore &profile);
size_t generatedControllerProfileCount();

} // namespace makcm::native
