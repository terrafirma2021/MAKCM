#pragma once

#include "controller_codec.h"

namespace makcm::native {

class EndpointRouter final {
public:
    bool configureExactStickProfile(
        const char *format_id, const char *cal_id,
        const makcm::stick::ControllerProfile &allowed,
        const makcm::stick::ControllerProfileIdentity &observed) {
        called = true;
        accepted_format_id = format_id;
        accepted_cal_id = cal_id;
        accepted_profile = allowed;
        accepted_identity = observed;
        return accept;
    }

    bool accept = true;
    bool called = false;
    const char *accepted_format_id = nullptr;
    const char *accepted_cal_id = nullptr;
    makcm::stick::ControllerProfile accepted_profile{};
    makcm::stick::ControllerProfileIdentity accepted_identity{};
};

} // namespace makcm::native
