#pragma once

#include <stdint.h>

#include "profile_store.h"

namespace makcm::native {

// Optional behavior lives here, outside the endpoint transport.  The router
// only asks whether a descriptor-derived endpoint is a mouse endpoint; it
// never interprets an endpoint number or VID/PID as a class.
// Unknown profiles remain fully ordered FIFO transports.
class ProxyPolicy final {
public:
    void configure(const ProfileStore *profile) {
        mouse_endpoint_ = 0;
        if (profile == nullptr) return;

        const EndpointInfo *mouse = profile->mouseEndpoint();
        if (mouse != nullptr) mouse_endpoint_ = mouse->address;

    }

    uint8_t mouseEndpointAddress() const { return mouse_endpoint_; }
    bool isMouseEndpoint(uint8_t address) const {
        return mouse_endpoint_ != 0 && address == mouse_endpoint_;
    }

private:
    uint8_t mouse_endpoint_ = 0;
};

} // namespace makcm::native
