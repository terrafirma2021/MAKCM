#include "raw_proxy_driver.h"

#include <algorithm>
#include <cstring>

#include "tusb.h"
#include "tinyusb_adapter.h"

namespace makcm::native {

RawProxyDriver *RawProxyDriver::active_ = nullptr;

namespace {
constexpr uint8_t kRequestDirectionIn = 0x80;
constexpr uint8_t kRequestTypeMask = 0x60;
constexpr uint8_t kRequestTypeStandard = 0x00;
constexpr uint8_t kRequestTypeClass = 0x20;
constexpr uint8_t kRecipientInterface = 0x01;
constexpr uint8_t kRecipientEndpoint = 0x02;
constexpr uint8_t kRequestGetStatus = 0x00;
constexpr uint8_t kRequestClearFeature = 0x01;
constexpr uint8_t kRequestSetFeature = 0x03;
constexpr uint8_t kRequestGetDescriptor = 0x06;
constexpr uint8_t kRequestGetInterface = 0x0A;
constexpr uint8_t kRequestSetInterface = 0x0B;
constexpr uint8_t kHidGetReport = 0x01;
constexpr uint8_t kHidGetIdle = 0x02;
constexpr uint8_t kHidGetProtocol = 0x03;
constexpr uint8_t kHidSetReport = 0x09;
constexpr uint8_t kHidSetIdle = 0x0A;
constexpr uint8_t kHidSetProtocol = 0x0B;
constexpr uint8_t kDescriptorReport = 0x22;
constexpr TickType_t kControlTimeoutTicks = pdMS_TO_TICKS(250);

bool isIn(const tinyusb::ControlRequestView &request) {
    return (request.bm_request_type & kRequestDirectionIn) != 0;
}

uint8_t recipient(const tinyusb::ControlRequestView &request) {
    return request.bm_request_type & 0x1F;
}

uint8_t requestType(const tinyusb::ControlRequestView &request) {
    return request.bm_request_type & kRequestTypeMask;
}

} // namespace

RawProxyDriver::~RawProxyDriver() {
    if (control_timer_ != nullptr) {
        xTimerDelete(control_timer_, 0);
        control_timer_ = nullptr;
    }
}

bool RawProxyDriver::begin(ProfileStore *profile, EndpointRouter *router,
                           BridgeTransport *bridge, uint32_t session,
                           Metrics *metrics) {
    if (profile == nullptr || router == nullptr || bridge == nullptr ||
        metrics == nullptr || !profile->ready()) return false;
    profile_ = profile;
    router_ = router;
    bridge_ = bridge;
    session_ = session;
    metrics_ = metrics;
    if (control_timer_ == nullptr) {
        control_timer_ = xTimerCreate("makcm_ctrl", kControlTimeoutTicks,
                                     pdFALSE, this,
                                     &RawProxyDriver::controlTimeoutThunk);
        if (control_timer_ == nullptr) return false;
    }
    reset();
    active_ = this;
    return true;
}

void RawProxyDriver::reset() {
    invalidateControl();
    std::memset(idle_, 0, sizeof(idle_));
    std::memset(protocol_, 1, sizeof(protocol_));
    if (router_ != nullptr) router_->reset(session_);
}

RawProxyDriver *RawProxyDriver::instance() { return active_; }

tinyusb::ControlRequestView RawProxyDriver::viewOf(
    const tusb_control_request_t &request) {
    tinyusb::ControlRequestView view;
    view.bm_request_type = request.bmRequestType;
    view.b_request = request.bRequest;
    view.w_value = request.wValue;
    view.w_index = request.wIndex;
    view.w_length = request.wLength;
    return view;
}

void RawProxyDriver::initCallback() {}
bool RawProxyDriver::deinitCallback() {
    if (active_ != nullptr) {
        active_->invalidateControl();
        // TinyUSB calls deinit for SET_CONFIGURATION(0) as well as during a
        // full driver uninstall.  In both cases the old endpoint tickets and
        // queued reports belong to the previous USB configuration and must
        // not survive into the next open cycle.  Resetting the router bumps
        // its epoch, so any late completion is classified stale instead of
        // retiring a report from the new configuration.
        if (active_->router_ != nullptr) {
            active_->router_->setUsbReady(false);
            active_->router_->reset(active_->session_);
        }
    }
    return true;
}

void RawProxyDriver::resetCallback(uint8_t rhport) {
    (void)rhport;
    if (active_ != nullptr) {
        active_->reset();
        if (active_->bridge_ != nullptr) {
            active_->bridge_->sendEmpty(makcm::bridge::MessageType::BusReset,
                                        active_->session_);
        }
    }
}

uint16_t RawProxyDriver::openCallback(
    uint8_t rhport, tusb_desc_interface_t const *desc_intf, uint16_t max_len) {
    return active_ == nullptr ? 0 : active_->open(rhport, desc_intf, max_len);
}

bool RawProxyDriver::controlCallback(uint8_t rhport, uint8_t stage,
                                     tusb_control_request_t const *request) {
    return active_ != nullptr && request != nullptr &&
           active_->control(rhport, stage, *request);
}

bool RawProxyDriver::xferCallback(uint8_t rhport, uint8_t ep_addr,
                                 xfer_result_t result,
                                 uint32_t xferred_bytes) {
    if (active_ == nullptr || active_->router_ == nullptr) return false;
    active_->router_->transferComplete(
        rhport, ep_addr, result == XFER_RESULT_SUCCESS, xferred_bytes);
    return true;
}

bool RawProxyDriver::xferIsrCallback(uint8_t rhport, uint8_t ep_addr,
                                     xfer_result_t result,
                                     uint32_t xferred_bytes) {
    // Never complete a transfer from this optional ISR hook.  The router
    // schedules deferred work and touches C++/FreeRTOS state, so it must run
    // through TinyUSB's normal task-context xfer_cb instead.
    (void)rhport;
    (void)ep_addr;
    (void)result;
    (void)xferred_bytes;
    return false;
}

void RawProxyDriver::sofCallback(uint8_t rhport, uint32_t frame_count) {
    (void)rhport;
    (void)frame_count;
}

uint16_t RawProxyDriver::open(uint8_t rhport,
                              tusb_desc_interface_t const *desc_intf,
                              uint16_t max_len) {
    open_calls_.fetch_add(1, std::memory_order_relaxed);
    last_open_interface_.store(desc_intf != nullptr ? desc_intf->bInterfaceNumber
                                                    : 0xFF,
                               std::memory_order_relaxed);
    last_open_expected_endpoints_.store(
        desc_intf != nullptr ? desc_intf->bNumEndpoints : 0,
        std::memory_order_relaxed);
    last_open_max_len_.store(max_len, std::memory_order_relaxed);
    last_open_consumed_.store(0, std::memory_order_relaxed);
    last_open_endpoint_.store(0, std::memory_order_relaxed);
    last_open_reason_.store(0, std::memory_order_relaxed);
    auto fail = [&](uint8_t reason) -> uint16_t {
        last_open_reason_.store(reason, std::memory_order_relaxed);
        open_failures_.fetch_add(1, std::memory_order_relaxed);
        return 0;
    };
    if (desc_intf == nullptr || max_len < desc_intf->bLength ||
        desc_intf->bLength < 9 || router_ == nullptr) return fail(1);
    const uint8_t *cursor = reinterpret_cast<const uint8_t *>(desc_intf);
    uint16_t consumed = desc_intf->bLength;
    uint8_t opened = 0;
    uint8_t opened_addresses[6] = {};
    auto rollback = [&]() {
        while (opened != 0) {
            --opened;
            router_->closeEndpoint(rhport, opened_addresses[opened]);
        }
    };
    // Consume the complete descriptor group belonging to this interface.
    // Vendor/class-specific descriptors may appear after the last endpoint;
    // returning at that endpoint makes TinyUSB treat the following vendor
    // descriptor as a new interface and abort SET_CONFIGURATION.  Stop only
    // at the next interface (or the end of the configuration), while opening
    // exactly the endpoints advertised by this interface.
    while (consumed + 2 <= max_len) {
        const uint8_t length = cursor[consumed];
        const uint8_t type = cursor[consumed + 1];
        if (length < 2 || consumed + length > max_len) {
            rollback();
            return fail(2);
        }
        if (type == TUSB_DESC_INTERFACE) break;
        if (type == TUSB_DESC_ENDPOINT) {
            if (opened >= desc_intf->bNumEndpoints ||
                opened >= sizeof(opened_addresses)) {
                rollback();
                return fail(3);
            }
            last_open_endpoint_.store(cursor[consumed + 2],
                                      std::memory_order_relaxed);
            if (length < 7 || !router_->openEndpoint(rhport,
                                                      cursor + consumed,
                                                       length)) {
                rollback();
                return fail(4);
            }
            opened_addresses[opened] = cursor[consumed + 2];
            ++opened;
        }
        consumed = static_cast<uint16_t>(consumed + length);
    }
    if (opened != desc_intf->bNumEndpoints) {
        rollback();
        return fail(5);
    }
    last_open_consumed_.store(consumed, std::memory_order_relaxed);
    last_open_reason_.store(0, std::memory_order_relaxed);
    open_successes_.fetch_add(1, std::memory_order_relaxed);
    return consumed;
}

bool RawProxyDriver::localControl(uint8_t rhport, uint8_t stage,
                                  const tinyusb::ControlRequestView &request) {
    if (stage != CONTROL_STAGE_SETUP) return false;
    const bool in = isIn(request);
    const uint8_t req_type = requestType(request);
    const uint8_t dest = recipient(request);
    if (in && profile_ != nullptr) {
        // Enumeration-time vendor descriptors are cached as part of the
        // profile.  Serving them locally removes the only EP0 path that can
        // otherwise be stretched by the UART/host round trip while Windows
        // is selecting a class driver.
        const CachedControlInfo *cached = profile_->cachedControl(
            request.bm_request_type, request.b_request, request.w_value,
            request.w_index);
        if (cached != nullptr && cached->data != nullptr &&
            cached->received_length != 0) {
            control_cache_hits_.fetch_add(1, std::memory_order_relaxed);
            const uint16_t length = std::min<uint16_t>(
                request.w_length, cached->received_length);
            return tinyusb::controlXfer(
                rhport, request, const_cast<uint8_t *>(cached->data), length);
        }
    }
    if (req_type == kRequestTypeStandard && dest == kRecipientInterface) {
        if (request.b_request == kRequestGetInterface && in &&
            request.w_length >= 1) {
            uint8_t value = 0;
            return tinyusb::controlXfer(rhport, request, &value, 1);
        }
        if (request.b_request == kRequestSetInterface && !in &&
            request.w_value == 0 && request.w_length == 0) {
            if (bridge_ != nullptr) {
                makcm::bridge::SetInterfaceHeader state;
                state.interfaceNumber =
                    static_cast<uint8_t>(request.w_index & 0xFF);
                state.alternateSetting =
                    static_cast<uint8_t>(request.w_value & 0xFF);
                bridge_->send(makcm::bridge::MessageType::SetInterface,
                              reinterpret_cast<const uint8_t *>(&state),
                              sizeof(state), session_);
            }
            return tinyusb::controlStatus(rhport, request);
        }
    }
    if (req_type == kRequestTypeStandard && dest == kRecipientEndpoint &&
        (request.b_request == kRequestGetStatus ||
         request.b_request == kRequestClearFeature ||
         request.b_request == kRequestSetFeature)) {
        // TinyUSB core owns endpoint halt/status.  Returning false lets it
        // execute the standard request rather than forwarding it to the host.
        return false;
    }
    if (req_type == kRequestTypeStandard && dest == kRecipientInterface &&
        request.b_request == kRequestGetDescriptor && in &&
        (request.w_value >> 8) == kDescriptorReport) {
        const uint8_t interface_number = static_cast<uint8_t>(request.w_index & 0xFF);
        const HidReportInfo *report = profile_->hidReport(interface_number);
        if (report == nullptr || report->data == nullptr ||
            report->received_length == 0) return false;
        const uint16_t length = std::min<uint16_t>(
            request.w_length, report->expected_length != 0
                ? report->expected_length : report->received_length);
        return tinyusb::controlXfer(rhport, request,
                                    const_cast<uint8_t *>(report->data), length);
    }
    if (req_type == kRequestTypeClass && dest == kRecipientInterface) {
        const uint8_t interface_number = static_cast<uint8_t>(request.w_index & 0xFF);
        if (interface_number < sizeof(idle_) && request.b_request == kHidGetIdle &&
            in && request.w_length >= 1) {
            return tinyusb::controlXfer(rhport, request, &idle_[interface_number], 1);
        }
        if (interface_number < sizeof(idle_) && request.b_request == kHidSetIdle &&
            !in && request.w_length == 0) {
            idle_[interface_number] = static_cast<uint8_t>(request.w_value >> 8);
            return tinyusb::controlStatus(rhport, request);
        }
        if (interface_number < sizeof(protocol_) &&
            request.b_request == kHidGetProtocol && in && request.w_length >= 1) {
            return tinyusb::controlXfer(rhport, request,
                                        &protocol_[interface_number], 1);
        }
        if (interface_number < sizeof(protocol_) &&
            request.b_request == kHidSetProtocol && !in &&
            request.w_length == 0) {
            protocol_[interface_number] = static_cast<uint8_t>(request.w_value);
            return tinyusb::controlStatus(rhport, request);
        }
        // GET_REPORT/SET_REPORT and vendor-specific interface requests are
        // forwarded below; their exact report semantics belong to the host.
        (void)kHidGetReport;
        (void)kHidSetReport;
    }
    return false;
}

bool RawProxyDriver::startForward(uint8_t rhport,
                                   const tinyusb::ControlRequestView &request,
                                   const uint8_t *out_data,
                                   uint16_t out_length) {
    constexpr uint16_t kHeader = sizeof(makcm::bridge::ControlRequestHeader);
    constexpr uint16_t kOutHeader =
        sizeof(makcm::bridge::ControlOutDataHeader);
    constexpr uint16_t kResponseHeader =
        sizeof(makcm::bridge::ControlResponseHeader);
    const bool direction_in = isIn(request);
    bool invalid = out_data == nullptr && out_length != 0;
    portENTER_CRITICAL(&control_mux_);
    const bool already_pending = control_.pending;
    portEXIT_CRITICAL(&control_mux_);
    const uint16_t payload_header = !direction_in && out_length != 0
        ? kOutHeader : kHeader;
    const uint16_t data_limit = makcm::bridge::kMaxPayloadSize - payload_header;
    if (already_pending || invalid || out_length > data_limit ||
        (!isIn(request) && out_length != request.w_length) ||
        (isIn(request) && request.w_length >
            makcm::bridge::kMaxPayloadSize - kResponseHeader)) {
        stallControl(rhport, direction_in);
        return false;
    }
    control_forwards_.fetch_add(1, std::memory_order_relaxed);
    const uint16_t id = next_request_id_.fetch_add(1, std::memory_order_relaxed);
    makcm::bridge::ControlRequestHeader header;
    header.requestId = id;
    header.bmRequestType = request.bm_request_type;
    header.bRequest = request.b_request;
    header.wValue = request.w_value;
    header.wIndex = request.w_index;
    header.wLength = request.w_length;
    header.dataLength = out_length;
    uint8_t payload[makcm::bridge::kMaxPayloadSize] = {};
    makcm::bridge::MessageType message_type =
        makcm::bridge::MessageType::ControlSetup;
    uint16_t payload_length = sizeof(header);
    if (!direction_in && out_length != 0) {
        makcm::bridge::ControlOutDataHeader out_header;
        out_header.request = header;
        out_header.offset = 0;
        out_header.totalLength = out_length;
        std::memcpy(payload, &out_header, sizeof(out_header));
        std::memcpy(payload + sizeof(out_header), out_data, out_length);
        message_type = makcm::bridge::MessageType::ControlOutData;
        payload_length = static_cast<uint16_t>(sizeof(out_header) + out_length);
    } else {
        std::memcpy(payload, &header, sizeof(header));
        if (out_length != 0)
            std::memcpy(payload + sizeof(header), out_data, out_length);
        payload_length = static_cast<uint16_t>(sizeof(header) + out_length);
    }
    portENTER_CRITICAL(&control_mux_);
    // Recheck after constructing the payload: a response/timeout can race a
    // previous transaction between the first check and this state install.
    if (control_.pending) {
        portEXIT_CRITICAL(&control_mux_);
        stallControl(rhport, direction_in);
        return false;
    }
    control_.pending = true;
    control_.direction_in = direction_in;
    control_.data_stage_pending = false;
    control_.status_stage_pending = !direction_in;
    control_.request_id = id;
    control_.expected_length = request.w_length;
    control_.request = request;
    control_.response_length = 0;
    portEXIT_CRITICAL(&control_mux_);
    // Arm the watchdog before sending.  This closes the small window where a
    // downstream response can arrive before the timer is started.
    if (control_timer_ == nullptr ||
        xTimerStart(control_timer_, 0) != pdPASS) {
        invalidateControl();
        stallControl(rhport, direction_in);
        return false;
    }
    if (!bridge_->send(message_type, payload, payload_length, session_)) {
        invalidateControl();
        stallControl(rhport, direction_in);
        return false;
    }
    metrics_->add(MetricId::ControlDeferred);
    return true;
}

bool RawProxyDriver::control(uint8_t rhport, uint8_t stage,
                             const tusb_control_request_t &request) {
    if (stage == CONTROL_STAGE_SETUP)
        control_setups_.fetch_add(1, std::memory_order_relaxed);
    const tinyusb::ControlRequestView view = viewOf(request);
    // TinyUSB invokes the registered callback again after completing the
    // status stage.  There is no additional proxy work to do at that point.
    if (stage == CONTROL_STAGE_ACK) return true;
    if (stage == CONTROL_STAGE_DATA) {
        // IN data was already supplied either from the local cache or by the
        // deferred downstream response.  Accept its completion so TinyUSB can
        // advance to the status stage instead of stalling EP0.
        if (isIn(view)) return true;
        uint8_t out_data[makcm::bridge::kMaxPayloadSize] = {};
        uint16_t out_length = 0;
        bool data_ready = false;
        portENTER_CRITICAL(&control_mux_);
        if (control_.pending && !control_.direction_in &&
            control_.data_stage_pending &&
            control_.expected_length <= sizeof(out_data)) {
            out_length = control_.expected_length;
            std::memcpy(out_data, control_.buffer, out_length);
            control_.data_stage_pending = false;
            control_.pending = false;
            data_ready = true;
        }
        portEXIT_CRITICAL(&control_mux_);
        if (data_ready)
            return startForward(rhport, view, out_data, out_length);
        portENTER_CRITICAL(&control_mux_);
        const bool pending = control_.pending;
        portEXIT_CRITICAL(&control_mux_);
        return pending;
    }
    if (stage != CONTROL_STAGE_SETUP) return false;
    portENTER_CRITICAL(&control_mux_);
    const bool pending = control_.pending;
    portEXIT_CRITICAL(&control_mux_);
    if (pending) return true;
    if (localControl(rhport, stage, view)) return true;
    if (!isIn(view) && view.w_length >
            makcm::bridge::kMaxPayloadSize -
                sizeof(makcm::bridge::ControlOutDataHeader)) {
        stallControl(rhport, false);
        return true;
    }
    if (!isIn(view) && view.w_length != 0) {
        portENTER_CRITICAL(&control_mux_);
        control_.pending = true;
        control_.direction_in = false;
        control_.data_stage_pending = true;
        control_.status_stage_pending = true;
        control_.request = view;
        control_.expected_length = view.w_length;
        portEXIT_CRITICAL(&control_mux_);
        // Roll back the provisional state if TinyUSB rejects the data stage.
        if (!tinyusb::controlXfer(rhport, view, control_.buffer,
                                  view.w_length)) {
            invalidateControl();
            stallControl(rhport, false);
        } else if (control_timer_ == nullptr ||
                   xTimerStart(control_timer_, 0) != pdPASS) {
            // The host may never advance to the DATA callback.  Arm the same
            // EP0 watchdog for this provisional stage so it cannot hold the
            // control pipe forever.
            invalidateControl();
            stallControl(rhport, false);
        }
        return true;
    }
    return startForward(rhport, view, nullptr, 0);
}

void RawProxyDriver::stallControl(uint8_t rhport, bool in_direction) {
    control_stalls_.fetch_add(1, std::memory_order_relaxed);
    tinyusb::controlStall(rhport, in_direction);
    if (metrics_ != nullptr) metrics_->add(MetricId::ControlTimeout);
}

void RawProxyDriver::invalidateControl() {
    portENTER_CRITICAL(&control_mux_);
    invalidateControlLocked();
    portEXIT_CRITICAL(&control_mux_);
    if (control_timer_ != nullptr) xTimerStop(control_timer_, 0);
}

void RawProxyDriver::invalidateControlLocked() {
    control_.pending = false;
    control_.direction_in = false;
    control_.data_stage_pending = false;
    control_.status_stage_pending = false;
    control_.request_id = 0;
    control_.expected_length = 0;
    control_.response_length = 0;
}

void RawProxyDriver::onControlResponse(const uint8_t *payload, uint16_t length) {
    if (payload == nullptr ||
        length < sizeof(makcm::bridge::ControlResponseHeader)) return;
    makcm::bridge::ControlResponseHeader header;
    std::memcpy(&header, payload, sizeof(header));
    bool direction_in = false;
    portENTER_CRITICAL(&control_mux_);
    if (!control_.pending || header.requestId != control_.request_id ||
        header.dataLength > length - sizeof(header) ||
        header.dataLength > sizeof(control_.buffer)) {
        portEXIT_CRITICAL(&control_mux_);
        return;
    }
    control_responses_.fetch_add(1, std::memory_order_relaxed);
    control_.response_length = header.dataLength;
    if (header.dataLength != 0)
        std::memcpy(control_.buffer, payload + sizeof(header), header.dataLength);
    control_.status_stage_pending = header.status ==
                                    makcm::bridge::ControlStatus::Completed;
    control_.data_stage_pending = header.status !=
                                   makcm::bridge::ControlStatus::Completed;
    direction_in = control_.direction_in;
    portEXIT_CRITICAL(&control_mux_);
    if (!tinyusb::defer(&RawProxyDriver::completeControlDeferred, this)) {
        ++control_timeouts_;
        scheduleControlStall(direction_in);
        invalidateControl();
    }
}

void RawProxyDriver::completeControlDeferred(void *context) {
    auto *driver = static_cast<RawProxyDriver *>(context);
    if (driver != nullptr) driver->completeControl();
}

void RawProxyDriver::stallControlDeferred(void *context) {
    auto *driver = static_cast<RawProxyDriver *>(context);
    if (driver == nullptr) return;
    const bool direction_in = driver->stall_direction_in_.load(
        std::memory_order_acquire);
    driver->stall_deferred_.store(false, std::memory_order_release);
    driver->stallControl(0, direction_in);
}

void RawProxyDriver::scheduleControlStall(bool in_direction) {
    stall_direction_in_.store(in_direction, std::memory_order_release);
    bool expected = false;
    if (!stall_deferred_.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return;
    }
    if (!tinyusb::defer(&RawProxyDriver::stallControlDeferred, this)) {
        stall_deferred_.store(false, std::memory_order_release);
    }
}

void RawProxyDriver::controlTimeoutThunk(TimerHandle_t timer) {
    auto *driver = static_cast<RawProxyDriver *>(pvTimerGetTimerID(timer));
    if (driver != nullptr) driver->controlTimeout();
}

void RawProxyDriver::controlTimeout() {
    bool pending = false;
    bool direction_in = false;
    portENTER_CRITICAL(&control_mux_);
    if (control_.pending) {
        pending = true;
        direction_in = control_.direction_in;
        invalidateControlLocked();
    }
    portEXIT_CRITICAL(&control_mux_);
    if (!pending) return;
    if (control_timer_ != nullptr) xTimerStop(control_timer_, 0);
    ++control_timeouts_;
    scheduleControlStall(direction_in);
}

void RawProxyDriver::completeControl() {
    tinyusb::ControlRequestView request{};
    bool direction_in = false;
    bool status_pending = false;
    bool data_pending = false;
    uint16_t response_length = 0;
    uint8_t response[makcm::bridge::kMaxPayloadSize] = {};
    portENTER_CRITICAL(&control_mux_);
    if (!control_.pending) {
        portEXIT_CRITICAL(&control_mux_);
        return;
    }
    request = control_.request;
    direction_in = control_.direction_in;
    status_pending = control_.status_stage_pending;
    data_pending = control_.data_stage_pending;
    response_length = std::min<uint16_t>(
        control_.response_length, control_.expected_length);
    if (response_length != 0)
        std::memcpy(response, control_.buffer, response_length);
    invalidateControlLocked();
    portEXIT_CRITICAL(&control_mux_);
    if (control_timer_ != nullptr) xTimerStop(control_timer_, 0);
    if (!status_pending && data_pending) {
        stallControl(0, direction_in);
        return;
    }
    if (direction_in) {
        if (!tinyusb::controlXfer(0, request, response, response_length))
            stallControl(0, true);
    } else if (!tinyusb::controlStatus(0, request)) {
        stallControl(0, false);
    }
}

bool RawProxyDriver::remoteWakeup() {
    if (metrics_ != nullptr) metrics_->add(MetricId::RemoteWake);
    return tinyusb::remoteWakeup();
}

usbd_class_driver_t const *RawProxyDriver::classDriver() {
    static const usbd_class_driver_t driver = {
        .name = "makcm_raw",
        .init = &RawProxyDriver::initCallback,
#if MAKCM_TINYUSB_API_021
        .deinit = &RawProxyDriver::deinitCallback,
#endif
        .reset = &RawProxyDriver::resetCallback,
        .open = &RawProxyDriver::openCallback,
        .control_xfer_cb = &RawProxyDriver::controlCallback,
        .xfer_cb = &RawProxyDriver::xferCallback,
#if MAKCM_TINYUSB_API_021
        // Keep completion in TinyUSB task context.  The optional ISR callback
        // is deliberately disabled because the router is not ISR-safe.
        .xfer_isr = nullptr,
#endif
        .sof = &RawProxyDriver::sofCallback,
    };
    return &driver;
}

} // namespace makcm::native

extern "C" usbd_class_driver_t const *usbd_app_driver_get_cb(
    uint8_t *driver_count) {
    if (driver_count != nullptr) *driver_count = 1;
    return makcm::native::RawProxyDriver::classDriver();
}

extern "C" bool tud_vendor_control_xfer_cb(
    uint8_t rhport, uint8_t stage,
    tusb_control_request_t const *request) {
    // usbd.c handles TUSB_REQ_TYPE_VENDOR before recipient-based class
    // dispatch.  Without this strong override, TinyUSB's weak callback
    // returns false and Windows never receives the cached MS OS descriptor.
    return makcm::native::RawProxyDriver::controlCallback(
        rhport, stage, request);
}
