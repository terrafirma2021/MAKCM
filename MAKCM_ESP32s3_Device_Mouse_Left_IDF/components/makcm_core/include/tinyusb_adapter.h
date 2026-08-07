#pragma once

#include <stddef.h>
#include <stdint.h>

namespace makcm::native::tinyusb {

struct ControlRequestView {
    uint8_t bm_request_type = 0;
    uint8_t b_request = 0;
    uint16_t w_value = 0;
    uint16_t w_index = 0;
    uint16_t w_length = 0;
};

using DeferredFunction = void (*)(void *context);

bool defer(DeferredFunction function, void *context, bool in_isr = false);
bool openEndpoint(uint8_t rhport, const uint8_t *endpoint_descriptor);
void closeEndpoint(uint8_t rhport, uint8_t endpoint_address);
bool claim(uint8_t rhport, uint8_t endpoint_address);
bool release(uint8_t rhport, uint8_t endpoint_address);
bool busy(uint8_t rhport, uint8_t endpoint_address);
bool stalled(uint8_t rhport, uint8_t endpoint_address);
void stall(uint8_t rhport, uint8_t endpoint_address);
void clearStall(uint8_t rhport, uint8_t endpoint_address);
bool submit(uint8_t rhport, uint8_t endpoint_address, uint8_t *buffer,
            uint16_t length);

bool controlXfer(uint8_t rhport, const ControlRequestView &request,
                 uint8_t *buffer, uint16_t length);
bool controlStatus(uint8_t rhport, const ControlRequestView &request);
bool controlStall(uint8_t rhport, bool in_direction);
bool remoteWakeup();

} // namespace makcm::native::tinyusb
