#pragma once

#include <atomic>
#include <stdint.h>

#include "BridgeProtocol.h"
#include "endpoint_router.h"
#include "profile_store.h"
#include "tinyusb_adapter.h"

// TinyUSB private types are intentionally confined to this adapter boundary.
#include "device/usbd_pvt.h"
#include "freertos/portmacro.h"
#include "freertos/timers.h"

#ifndef MAKCM_TINYUSB_API_021
#define MAKCM_TINYUSB_API_021 1
#endif

namespace makcm::native {

class RawProxyDriver final {
public:
    RawProxyDriver() = default;
    ~RawProxyDriver();
    RawProxyDriver(const RawProxyDriver &) = delete;
    RawProxyDriver &operator=(const RawProxyDriver &) = delete;

    bool begin(ProfileStore *profile, EndpointRouter *router,
               BridgeTransport *bridge, uint32_t session, Metrics *metrics);
    void reset();
    void onControlResponse(const uint8_t *payload, uint16_t length);
    bool remoteWakeup();
    uint32_t controlTimeouts() const { return control_timeouts_.load(); }
    uint32_t controlSetups() const { return control_setups_.load(); }
    uint32_t controlForwards() const { return control_forwards_.load(); }
    uint32_t controlResponses() const { return control_responses_.load(); }
    uint32_t controlStalls() const { return control_stalls_.load(); }
    uint32_t controlCacheHits() const { return control_cache_hits_.load(); }
    uint32_t openCalls() const { return open_calls_.load(); }
    uint32_t openSuccesses() const { return open_successes_.load(); }
    uint32_t openFailures() const { return open_failures_.load(); }
    uint8_t lastOpenInterface() const { return last_open_interface_.load(); }
    uint8_t lastOpenExpectedEndpoints() const {
        return last_open_expected_endpoints_.load();
    }
    uint8_t lastOpenEndpoint() const { return last_open_endpoint_.load(); }
    uint8_t lastOpenReason() const { return last_open_reason_.load(); }
    uint16_t lastOpenMaxLen() const { return last_open_max_len_.load(); }
    uint16_t lastOpenConsumed() const { return last_open_consumed_.load(); }

    static void initCallback();
    static bool deinitCallback();
    static void resetCallback(uint8_t rhport);
    static uint16_t openCallback(uint8_t rhport,
                                 tusb_desc_interface_t const *desc_intf,
                                 uint16_t max_len);
    static bool controlCallback(uint8_t rhport, uint8_t stage,
                                tusb_control_request_t const *request);
    static bool xferCallback(uint8_t rhport, uint8_t ep_addr,
                             xfer_result_t result, uint32_t xferred_bytes);
    static bool xferIsrCallback(uint8_t rhport, uint8_t ep_addr,
                                xfer_result_t result, uint32_t xferred_bytes);
    static void sofCallback(uint8_t rhport, uint32_t frame_count);

    static usbd_class_driver_t const *classDriver();
    static RawProxyDriver *instance();

private:
    struct ControlState {
        bool pending = false;
        bool direction_in = false;
        bool data_stage_pending = false;
        bool status_stage_pending = false;
        uint16_t request_id = 0;
        uint16_t expected_length = 0;
        tinyusb::ControlRequestView request{};
        alignas(4) uint8_t buffer[makcm::bridge::kMaxPayloadSize]{};
        uint16_t response_length = 0;
    };

    uint16_t open(uint8_t rhport, tusb_desc_interface_t const *desc_intf,
                  uint16_t max_len);
    bool control(uint8_t rhport, uint8_t stage,
                 tusb_control_request_t const &request);
    bool localControl(uint8_t rhport, uint8_t stage,
                      const tinyusb::ControlRequestView &request);
    bool startForward(uint8_t rhport,
                      const tinyusb::ControlRequestView &request,
                      const uint8_t *out_data, uint16_t out_length);
    static void completeControlDeferred(void *context);
    static void stallControlDeferred(void *context);
    static void controlTimeoutThunk(TimerHandle_t timer);
    void completeControl();
    void controlTimeout();
    void stallControl(uint8_t rhport, bool in_direction);
    void scheduleControlStall(bool in_direction);
    void invalidateControl();
    void invalidateControlLocked();
    static tinyusb::ControlRequestView viewOf(
        const tusb_control_request_t &request);

    static RawProxyDriver *active_;
    ProfileStore *profile_ = nullptr;
    EndpointRouter *router_ = nullptr;
    BridgeTransport *bridge_ = nullptr;
    Metrics *metrics_ = nullptr;
    uint32_t session_ = 0;
    std::atomic<uint16_t> next_request_id_{1};
    std::atomic<uint32_t> control_timeouts_{0};
    std::atomic<uint32_t> control_setups_{0};
    std::atomic<uint32_t> control_forwards_{0};
    std::atomic<uint32_t> control_responses_{0};
    std::atomic<uint32_t> control_stalls_{0};
    std::atomic<uint32_t> control_cache_hits_{0};
    std::atomic<bool> stall_deferred_{false};
    std::atomic<bool> stall_direction_in_{false};
    std::atomic<uint32_t> open_calls_{0};
    std::atomic<uint32_t> open_successes_{0};
    std::atomic<uint32_t> open_failures_{0};
    std::atomic<uint8_t> last_open_interface_{0xFF};
    std::atomic<uint8_t> last_open_expected_endpoints_{0};
    std::atomic<uint8_t> last_open_endpoint_{0};
    std::atomic<uint8_t> last_open_reason_{0};
    std::atomic<uint16_t> last_open_max_len_{0};
    std::atomic<uint16_t> last_open_consumed_{0};
    ControlState control_{};
    portMUX_TYPE control_mux_ = portMUX_INITIALIZER_UNLOCKED;
    TimerHandle_t control_timer_ = nullptr;
    uint8_t idle_[16]{};
    uint8_t protocol_[16]{};
};

} // namespace makcm::native

extern "C" usbd_class_driver_t const *usbd_app_driver_get_cb(
    uint8_t *driver_count);

// TinyUSB dispatches every vendor-type EP0 request here before examining its
// recipient.  Keep this global hook connected to the same proxy control path
// as interface/class requests.
extern "C" bool tud_vendor_control_xfer_cb(
    uint8_t rhport, uint8_t stage,
    tusb_control_request_t const *request);
