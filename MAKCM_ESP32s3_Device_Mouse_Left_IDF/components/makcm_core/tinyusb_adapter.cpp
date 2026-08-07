#include "tinyusb_adapter.h"

#include <new>

#include "device/usbd_pvt.h"
#include "tusb.h"

namespace makcm::native::tinyusb {

namespace {
struct DeferredContext {
    DeferredFunction function = nullptr;
    void *context = nullptr;
};

void deferredThunk(void *arg) {
    auto *deferred = static_cast<DeferredContext *>(arg);
    if (deferred != nullptr && deferred->function != nullptr)
        deferred->function(deferred->context);
    delete deferred;
}

tusb_control_request_t toTinyusb(const ControlRequestView &request) {
    tusb_control_request_t result{};
    result.bmRequestType = request.bm_request_type;
    result.bRequest = request.b_request;
    result.wValue = request.w_value;
    result.wIndex = request.w_index;
    result.wLength = request.w_length;
    return result;
}
} // namespace

bool defer(DeferredFunction function, void *context, bool in_isr) {
    if (function == nullptr) return false;
    auto *deferred = new (std::nothrow) DeferredContext{function, context};
    if (deferred == nullptr) return false;
    usbd_defer_func(&deferredThunk, deferred, in_isr);
    return true;
}

bool openEndpoint(uint8_t rhport, const uint8_t *endpoint_descriptor) {
    return endpoint_descriptor != nullptr &&
           usbd_edpt_open(
               rhport,
               reinterpret_cast<tusb_desc_endpoint_t const *>(
                   endpoint_descriptor));
}

void closeEndpoint(uint8_t rhport, uint8_t endpoint_address) {
    usbd_edpt_close(rhport, endpoint_address);
}

bool claim(uint8_t rhport, uint8_t endpoint_address) {
    return usbd_edpt_claim(rhport, endpoint_address);
}

bool release(uint8_t rhport, uint8_t endpoint_address) {
    return usbd_edpt_release(rhport, endpoint_address);
}

bool busy(uint8_t rhport, uint8_t endpoint_address) {
    return usbd_edpt_busy(rhport, endpoint_address);
}

bool stalled(uint8_t rhport, uint8_t endpoint_address) {
    return usbd_edpt_stalled(rhport, endpoint_address);
}

void stall(uint8_t rhport, uint8_t endpoint_address) {
    usbd_edpt_stall(rhport, endpoint_address);
}

void clearStall(uint8_t rhport, uint8_t endpoint_address) {
    usbd_edpt_clear_stall(rhport, endpoint_address);
}

bool submit(uint8_t rhport, uint8_t endpoint_address, uint8_t *buffer,
            uint16_t length) {
    // TinyUSB 0.21's DCD API carries an explicit ISR flag.  This wrapper is
    // the only place in the project that knows that private ABI detail.
    if (!usbd_edpt_claim(rhport, endpoint_address)) return false;
    const bool submitted =
        usbd_edpt_xfer(rhport, endpoint_address, buffer, length, false);
    usbd_edpt_release(rhport, endpoint_address);
    return submitted;
}

bool controlXfer(uint8_t rhport, const ControlRequestView &request,
                 uint8_t *buffer, uint16_t length) {
    auto tinyusb_request = toTinyusb(request);
    return tud_control_xfer(rhport, &tinyusb_request, buffer, length);
}

bool controlStatus(uint8_t rhport, const ControlRequestView &request) {
    auto tinyusb_request = toTinyusb(request);
    return tud_control_status(rhport, &tinyusb_request);
}

bool controlStall(uint8_t rhport, bool in_direction) {
    usbd_edpt_stall(rhport, in_direction ? 0x80 : 0x00);
    return true;
}

bool remoteWakeup() { return tud_remote_wakeup(); }

} // namespace makcm::native::tinyusb
