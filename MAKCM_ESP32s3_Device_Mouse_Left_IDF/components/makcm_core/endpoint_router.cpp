#include "endpoint_router.h"

#include <algorithm>
#include <cstring>

#include "esp_timer.h"
#include "tinyusb_adapter.h"

namespace makcm::native {

EndpointRouter::EndpointRouter() {
    // The Secondary UART task starts before a controller profile can call
    // begin().  Create the statically allocated mutex during global object
    // construction so early km.move/km.caps traffic remains a safe no-op
    // instead of touching an uninitialized lock.
    const bool initialized = initializeMutex();
    configASSERT(initialized);
    (void)initialized;
}

bool EndpointRouter::initializeMutex() {
    if (mux_ != nullptr) return true;
    mux_ = xSemaphoreCreateRecursiveMutexStatic(&mux_storage_);
    return mux_ != nullptr;
}

void EndpointRouter::lock() const {
    configASSERT(mux_ != nullptr);
    const BaseType_t taken = xSemaphoreTakeRecursive(mux_, portMAX_DELAY);
    configASSERT(taken == pdTRUE);
    (void)taken;
}

void EndpointRouter::unlock() const {
    configASSERT(mux_ != nullptr);
    const BaseType_t given = xSemaphoreGiveRecursive(mux_);
    configASSERT(given == pdTRUE);
    (void)given;
}

void EndpointRouter::bindStickPhysical(uint32_t session, uint64_t now_ms) {
    if (stick_runtime_ == nullptr) return;
    lock();
    stick_runtime_->bindPhysical(session, now_ms);
    unlock();
}

void EndpointRouter::hotplugStick(uint64_t now_ms) {
    if (stick_runtime_ == nullptr) return;
    lock();
    stick_runtime_->hotplug(now_ms);
    unlock();
}

bool EndpointRouter::handleStickLine(const char *line, uint64_t now_ms,
                                     char *response,
                                     size_t response_capacity) {
    if (stick_runtime_ == nullptr) return false;
    lock();
    const bool handled = stick_runtime_->handleLine(
        line, now_ms, response, response_capacity);
    unlock();
    return handled;
}

bool EndpointRouter::takeStickResponse(char *response,
                                       size_t response_capacity) {
    if (stick_runtime_ == nullptr) return false;
    lock();
    const bool available = stick_runtime_->takePendingResponse(
        response, response_capacity);
    unlock();
    return available;
}

bool EndpointRouter::pollStickWatchdog(uint64_t now_ms) {
    if (stick_runtime_ == nullptr) return false;
    lock();
    stick_runtime_->checkStreamWatchdog(now_ms);
    const bool detach = stick_runtime_->takeDetachRequest();
    unlock();
    return detach;
}

StickRuntimeSnapshot EndpointRouter::stickSnapshot(uint64_t now_ms) {
    StickRuntimeSnapshot snapshot;
    if (stick_runtime_ == nullptr) return snapshot;
    lock();
    snapshot = stick_runtime_->snapshot(now_ms);
    unlock();
    return snapshot;
}

bool EndpointRouter::configureExactStickProfile(
    const char *format_id, const char *cal_id,
    const makcm::stick::ControllerProfile &allowed,
    const makcm::stick::ControllerProfileIdentity &observed) {
    if (stick_runtime_ == nullptr) return false;
    lock();
    const bool configured = stick_runtime_->configureExactProfile(
        format_id, cal_id, allowed, observed);
    unlock();
    return configured;
}

bool EndpointRouter::begin(ProfileStore *profile, BridgeTransport *bridge,
                           uint32_t session, Metrics *metrics) {
    if (profile == nullptr || bridge == nullptr || metrics == nullptr ||
        !profile->ready()) return false;
    if (!initializeMutex()) return false;
    profile_ = profile;
    bridge_ = bridge;
    metrics_ = metrics;
    session_ = session;
    usb_ready_.store(false, std::memory_order_release);
    policy_.configure(profile_);
    lock();
    mouse_layout_ = {};
    mouse_ledger_.reset();
    unlock();
    injection_moves_accepted_.store(0, std::memory_order_relaxed);
    injection_moves_rejected_.store(0, std::memory_order_relaxed);
    injection_buttons_accepted_.store(0, std::memory_order_relaxed);
    injection_buttons_rejected_.store(0, std::memory_order_relaxed);
    mouse_layout_valid_.store(false, std::memory_order_release);
    mouse_layout_report_id_.store(0, std::memory_order_relaxed);
    mouse_layout_report_length_.store(0, std::memory_order_relaxed);
    reset(session);
    return true;
}

void EndpointRouter::reset(uint32_t session) {
    session_ = session;
    const uint32_t next_epoch = epoch_.fetch_add(1, std::memory_order_acq_rel) + 1;
    lock();
    for (auto &endpoint : endpoints_) {
        endpoint = {};
        endpoint.epoch = next_epoch;
    }
    physical_fifo_head_ = 0;
    physical_fifo_tail_ = 0;
    physical_fifo_count_ = 0;
    std::memset(physical_fifo_lengths_, 0, sizeof(physical_fifo_lengths_));
    std::memset(physical_fifo_generations_, 0,
                sizeof(physical_fifo_generations_));
    physical_generation_ = 0;
    ready_mask_.store(0, std::memory_order_release);
    mouse_ledger_.markHostReset();
    unlock();
    drain_scheduled_.store(false, std::memory_order_release);
}

bool EndpointRouter::allEndpointsOpen() const {
    if (profile_ == nullptr || !profile_->ready() ||
        profile_->endpointCount() == 0) return false;
    lock();
    bool complete = true;
    for (uint8_t i = 0; i < profile_->endpointCount(); ++i) {
        const EndpointInfo *info = profile_->endpoints() + i;
        const EndpointContext *endpoint = context(info->address);
        if (endpoint == nullptr || !endpoint->configured ||
            endpoint->phase.phase() == makcm::usb::EndpointPhase::Closed) {
            complete = false;
            break;
        }
    }
    unlock();
    return complete;
}

void EndpointRouter::setUsbReady(bool ready) {
    usb_ready_.store(ready, std::memory_order_release);
    if (ready && ready_mask_.load(std::memory_order_acquire) != 0) {
        scheduleDrain();
    }
}

uint8_t EndpointRouter::contextIndex(uint8_t address) {
    const uint8_t number = address & 0x0F;
    const uint8_t direction = (address & 0x80) != 0 ? 1 : 0;
    return static_cast<uint8_t>(direction * (kMaxEndpointNumber + 1) + number);
}

EndpointRouter::EndpointContext *EndpointRouter::context(uint8_t address) {
    const uint8_t number = address & 0x0F;
    if (number == 0 || number > kMaxEndpointNumber) return nullptr;
    return &endpoints_[contextIndex(address)];
}

const EndpointRouter::EndpointContext *EndpointRouter::context(
    uint8_t address) const {
    const uint8_t number = address & 0x0F;
    if (number == 0 || number > kMaxEndpointNumber) return nullptr;
    return &endpoints_[contextIndex(address)];
}

void EndpointRouter::markReady(uint8_t address) {
    const uint8_t number = address & 0x0F;
    if (number == 0 || number > kMaxEndpointNumber) return;
    if (metrics_ != nullptr) metrics_->add(MetricId::EndpointReady);
    ready_mask_.fetch_or(1u << contextIndex(address), std::memory_order_release);
    scheduleDrain();
}

void EndpointRouter::scheduleDrain() {
    if (!usb_ready_.load(std::memory_order_acquire)) return;
    bool expected = false;
    if (!drain_scheduled_.compare_exchange_strong(expected, true,
                                                  std::memory_order_acq_rel)) {
        return;
    }
    if (!tinyusb::defer(&EndpointRouter::deferredDrain, this)) {
        drain_scheduled_.store(false, std::memory_order_release);
    }
}

void EndpointRouter::deferredDrain(void *context) {
    auto *router = static_cast<EndpointRouter *>(context);
    if (router != nullptr) router->drain(0);
}

bool EndpointRouter::openEndpoint(uint8_t rhport, const uint8_t *descriptor,
                                  uint16_t descriptor_length) {
    open_attempts_.fetch_add(1, std::memory_order_relaxed);
    auto fail = [&](uint8_t stage) -> bool {
        last_open_failure_stage_.store(stage, std::memory_order_relaxed);
        open_failures_.fetch_add(1, std::memory_order_relaxed);
        return false;
    };
    if (descriptor == nullptr || descriptor_length < 7 ||
        (descriptor[2] & 0x0F) == 0 || (descriptor[2] & 0x0F) >
            kMaxEndpointNumber || (descriptor[3] & 0x03) == 1) {
        if (descriptor != nullptr) {
            last_open_failure_endpoint_.store(descriptor[2],
                                              std::memory_order_relaxed);
        }
        return fail(1);
    }
    const uint8_t address = descriptor[2];
    last_open_failure_endpoint_.store(address, std::memory_order_relaxed);
    EndpointContext *endpoint = context(address);
    if (endpoint == nullptr) {
        return fail(2);
    }
    lock();
    if (!tinyusb::openEndpoint(rhport, descriptor)) {
        unlock();
        return fail(3);
    }
    endpoint->configured = true;
    endpoint->in_direction = (address & 0x80) != 0;
    endpoint->address = address;
    endpoint->max_packet = static_cast<uint16_t>(descriptor[4]) |
                           (static_cast<uint16_t>(descriptor[5]) << 8);
    endpoint->max_packet &= 0x07FF;
    endpoint->interval = descriptor[6];
    endpoint->epoch = epoch();
    endpoint->phase.reset();
    endpoint->phase.open(endpoint->epoch);
    if (endpoint->max_packet == 0 || endpoint->max_packet > kBufferSize) {
        tinyusb::closeEndpoint(rhport, address);
        endpoint->configured = false;
        endpoint->phase.reset();
        unlock();
        return fail(4);
    }
    if (!endpoint->in_direction && !submitOut(*endpoint, rhport)) {
        tinyusb::closeEndpoint(rhport, address);
        endpoint->configured = false;
        endpoint->phase.reset();
        unlock();
        return fail(5);
    }
    unlock();
    return true;
}

void EndpointRouter::closeEndpoint(uint8_t rhport, uint8_t address) {
    EndpointContext *endpoint = context(address);
    if (endpoint == nullptr) return;
    lock();
    tinyusb::closeEndpoint(rhport, address);
    endpoint->phase.reset();
    endpoint->configured = false;
    ready_mask_.fetch_and(~(1u << contextIndex(address)), std::memory_order_release);
    unlock();
}

bool EndpointRouter::queueEndpointInput(uint8_t address, const uint8_t *data,
                                        uint16_t length) {
    if ((address & 0x80) == 0 || data == nullptr || length == 0 ||
        length > kBufferSize) return false;
    EndpointContext *endpoint = context(address);
    if (endpoint == nullptr) return false;

    lock();
    // Input is never allowed to create an endpoint implicitly.  The RAW
    // driver's open callback is the sole owner of endpoint configuration;
    // this prevents a stream frame racing SET_CONFIGURATION from submitting
    // to an address that TinyUSB has not opened yet.
    if (!endpoint->configured || endpoint->phase.phase() ==
            makcm::usb::EndpointPhase::Closed) {
        unlock();
        return false;
    }
    if (policy_.isMouseEndpoint(address)) {
        if (physical_fifo_count_ >= kQueueDepth) {
            metrics_->add(MetricId::EndpointDrop);
            unlock();
            return false;
        }
        const uint8_t slot = physical_fifo_tail_;
        std::memcpy(physical_fifo_[slot], data, length);
        physical_fifo_lengths_[slot] = length;
        physical_fifo_generations_[slot] = ++physical_generation_;
        physical_fifo_tail_ = static_cast<uint8_t>((slot + 1) % kQueueDepth);
        ++physical_fifo_count_;
    } else {
        if (endpoint->fifo_count >= kQueueDepth) {
            if (stick_runtime_ != nullptr)
                stick_runtime_->observeFifo(address, length, data[0],
                                            endpoint->fifo_count, true);
            metrics_->add(MetricId::EndpointDrop);
            unlock();
            return false;
        }
        const uint8_t slot = endpoint->fifo_tail;
        std::memcpy(endpoint->fifo[slot], data, length);
        endpoint->fifo_lengths[slot] = length;
        endpoint->fifo_tail = static_cast<uint8_t>((slot + 1) % kQueueDepth);
        ++endpoint->fifo_count;
        if (stick_runtime_ != nullptr)
            stick_runtime_->observeFifo(address, length, data[0],
                                        endpoint->fifo_count, false);
    }
    unlock();
    markReady(address);
    return true;
}

bool EndpointRouter::chooseMouseReport(EndpointContext &endpoint) {
    if (!mouse_layout_.valid) {
        // A generic HID mouse whose report grammar is not understood must
        // still be transparently forwarded.  The policy layer simply has no
        // injection capability for it; it must not turn a valid physical
        // report into a permanent queue stall.
        if (physical_fifo_count_ == 0) return false;
        const uint8_t slot = physical_fifo_head_;
        const uint16_t length = physical_fifo_lengths_[slot];
        if (length == 0 || length > endpoint.max_packet) return false;
        std::memcpy(endpoint.compose, physical_fifo_[slot], length);
        endpoint.inflight_length = length;
        endpoint.source = Source::MousePhysical;
        endpoint.source_slot = slot;
        endpoint.source_generation = physical_fifo_generations_[slot];
        return true;
    }
    if (mouse_ledger_.reservationActive()) return false;
    const auto snapshot = mouse_ledger_.snapshot();
    const bool ledger_pending = mouse_ledger_.pending();
    const bool physical_new = physical_fifo_count_ != 0;
    if (!physical_new && !ledger_pending) return false;
    if (!mouse_layout_.valid) return false;

    int64_t x = snapshot.x;
    int64_t y = snapshot.y;
    int64_t wheel = snapshot.wheel;
    int64_t pan = snapshot.pan;
    makcm::hid::MouseMergeDelta consumed{};
    const uint8_t physical_slot = physical_fifo_head_;
    const uint16_t length = makcm::hid::mergeMouseReport(
        mouse_layout_, physical_new ? physical_fifo_[physical_slot] : nullptr,
        physical_new ? physical_fifo_lengths_[physical_slot] : 0, physical_new,
        snapshot.buttons, x, y, wheel, pan, endpoint.compose,
        endpoint.max_packet, &consumed);
    if (length == 0) return false;
    if (ledger_pending &&
        !mouse_ledger_.reserve(consumed, snapshot.buttonGeneration)) {
        return false;
    }
    endpoint.inflight_length = length;
    endpoint.source = ledger_pending ? Source::MouseLedger
                                         : Source::MousePhysical;
    endpoint.source_slot = physical_slot;
    endpoint.source_generation = physical_new
                                     ? physical_fifo_generations_[physical_slot]
                                     : 0;
    return true;
}

bool EndpointRouter::chooseReport(EndpointContext &endpoint) {
    if (policy_.isMouseEndpoint(endpoint.address))
        return chooseMouseReport(endpoint);
    if (endpoint.fifo_count == 0) return false;
    const uint8_t slot = endpoint.fifo_head;
    const uint16_t length = endpoint.fifo_lengths[slot];
    if (length == 0 || length > endpoint.max_packet) return false;
    std::memcpy(endpoint.compose, endpoint.fifo[slot], length);
    endpoint.stick_compose = {};
    if (stick_runtime_ != nullptr) {
        endpoint.stick_compose = stick_runtime_->compose(
            endpoint.address, endpoint.fifo[slot], endpoint.compose, length,
            static_cast<uint64_t>(esp_timer_get_time() / 1000ULL));
    }
    endpoint.inflight_length = length;
    endpoint.source = Source::Fifo;
    endpoint.source_slot = slot;
    return true;
}

bool EndpointRouter::submit(EndpointContext &endpoint, uint8_t rhport) {
    if (!endpoint.configured || !endpoint.in_direction ||
        endpoint.phase.phase() != makcm::usb::EndpointPhase::Idle ||
        !chooseReport(endpoint)) return false;
    const uint32_t current_epoch = epoch();
    if (!endpoint.phase.begin(current_epoch)) {
        const Source source = endpoint.source;
        endpoint.source = Source::None;
        endpoint.stick_compose = {};
        if (source == Source::MouseLedger) mouse_ledger_.settle(false);
        return false;
    }
    std::memcpy(endpoint.inflight, endpoint.compose, endpoint.inflight_length);
    if (!endpoint.phase.arm(current_epoch, endpoint.inflight_length)) {
        const Source source = endpoint.source;
        endpoint.source = Source::None;
        endpoint.stick_compose = {};
        if (source == Source::MouseLedger) mouse_ledger_.settle(false);
        return false;
    }
    if (metrics_ != nullptr) metrics_->add(MetricId::EndpointSubmit);
    if (!tinyusb::submit(rhport, endpoint.address, endpoint.inflight,
                         endpoint.inflight_length)) {
        endpoint.phase.submitFailed(current_epoch);
        finishSource(endpoint, false);
        if (metrics_ != nullptr) metrics_->add(MetricId::EndpointRetry);
        return false;
    }
    return true;
}

bool EndpointRouter::submitOut(EndpointContext &endpoint, uint8_t rhport) {
    if (!endpoint.configured || endpoint.in_direction ||
        endpoint.phase.phase() != makcm::usb::EndpointPhase::Idle) return false;
    const uint32_t current_epoch = epoch();
    if (!endpoint.phase.begin(current_epoch) ||
        !endpoint.phase.arm(current_epoch, endpoint.max_packet)) {
        return false;
    }
    endpoint.source = Source::Out;
    if (!tinyusb::submit(rhport, endpoint.address, endpoint.out_buffer,
                         endpoint.max_packet)) {
        endpoint.phase.submitFailed(current_epoch);
        endpoint.source = Source::None;
        if (metrics_ != nullptr) metrics_->add(MetricId::EndpointRetry);
        return false;
    }
    metrics_->add(MetricId::OutRearm);
    return true;
}

void EndpointRouter::finishSource(EndpointContext &endpoint, bool success) {
    if (stick_runtime_ != nullptr && endpoint.source == Source::Fifo) {
        stick_runtime_->reportCompleted(
            endpoint.address, success, endpoint.stick_compose,
            static_cast<uint64_t>(esp_timer_get_time() / 1000ULL));
    }
    switch (endpoint.source) {
    case Source::Fifo:
        if (success && endpoint.fifo_count != 0) {
            endpoint.fifo_head = static_cast<uint8_t>(
                (endpoint.fifo_head + 1) % kQueueDepth);
            --endpoint.fifo_count;
        }
        break;
    case Source::MousePhysical:
        if (success && physical_fifo_count_ != 0 &&
            physical_fifo_head_ == endpoint.source_slot) {
            physical_fifo_head_ = static_cast<uint8_t>(
                (physical_fifo_head_ + 1) % kQueueDepth);
            --physical_fifo_count_;
        }
        break;
    case Source::MouseLedger:
        mouse_ledger_.settle(success);
        if (success && physical_fifo_count_ != 0 &&
            physical_fifo_head_ == endpoint.source_slot) {
            physical_fifo_head_ = static_cast<uint8_t>(
                (physical_fifo_head_ + 1) % kQueueDepth);
            --physical_fifo_count_;
        }
        break;
    case Source::Out:
    case Source::None:
        break;
    }
    endpoint.source = Source::None;
    endpoint.stick_compose = {};
}

bool EndpointRouter::hasPending(const EndpointContext &endpoint) const {
    if (!endpoint.configured) return false;
    if (!endpoint.in_direction) return endpoint.out_pending;
    if (policy_.isMouseEndpoint(endpoint.address)) {
        return physical_fifo_count_ != 0 ||
               (mouse_layout_.valid && mouse_ledger_.pending());
    }
    return endpoint.fifo_count != 0;
}

void EndpointRouter::sendOut(EndpointContext &endpoint) {
    if (!endpoint.out_pending || bridge_ == nullptr) return;
    uint8_t payload[2 + kBufferSize] = {};
    payload[0] = endpoint.address;
    payload[1] = 0;
    std::memcpy(payload + 2, endpoint.out_buffer, endpoint.out_length);
    if (bridge_->send(makcm::bridge::MessageType::EndpointOutData, payload,
                      static_cast<uint16_t>(endpoint.out_length + 2), session_)) {
        endpoint.out_pending = false;
        endpoint.out_length = 0;
    }
}

void EndpointRouter::transferComplete(uint8_t rhport, uint8_t address,
                                       bool success, uint32_t transferred) {
    EndpointContext *endpoint = context(address);
    if (endpoint == nullptr) return;
    lock();
    const uint32_t current_epoch = epoch();
    const bool exact = endpoint->in_direction;
    const auto completion = endpoint->phase.beginCompletion(
        current_epoch, success, transferred, exact);
    if (completion == makcm::usb::EndpointCompletion::Stale) {
        if (metrics_ != nullptr) metrics_->add(MetricId::EndpointStale);
        unlock();
        return;
    }
    if (!endpoint->in_direction && success && transferred <= kBufferSize) {
        endpoint->out_length = static_cast<uint16_t>(transferred);
        endpoint->out_pending = transferred != 0;
    }
    const bool delivered = completion == makcm::usb::EndpointCompletion::Delivered;
    if (!delivered && metrics_ != nullptr) metrics_->add(MetricId::EndpointRetry);
    const bool report_failure = !success && bridge_ != nullptr;
    finishSource(*endpoint, delivered);
    endpoint->phase.finishCompletion(current_epoch);
    if (metrics_ != nullptr) metrics_->add(MetricId::EndpointComplete);
    const bool rearm = !endpoint->in_direction || hasPending(*endpoint);
    unlock();
    if (report_failure) {
        makcm::bridge::EndpointStatusHeader status;
        status.endpoint = address;
        status.flags = makcm::bridge::kEndpointFlagTransferError;
        bridge_->send(makcm::bridge::MessageType::StallHalt,
                      reinterpret_cast<const uint8_t *>(&status),
                      sizeof(status), session_);
    }
    if (rearm) markReady(address);
    (void)rhport;
}

void EndpointRouter::drain(uint8_t rhport) {
    drain_scheduled_.store(false, std::memory_order_release);
    // A deferred drain can already be queued when the host suspends the
    // bus.  Keep the ready bits intact, but do not consume them or submit a
    // DCD transfer until RESUMED re-enables the executor.
    if (!usb_ready_.load(std::memory_order_acquire)) return;
    const uint32_t snapshot = ready_mask_.exchange(0, std::memory_order_acq_rel);
    lock();
    for (uint8_t index = 0; index < endpoints_.size(); ++index) {
        if ((snapshot & (1u << index)) == 0) continue;
        EndpointContext &endpoint = endpoints_[index];
        if (!endpoint.configured || endpoint.phase.phase() !=
                makcm::usb::EndpointPhase::Idle) continue;
        if (endpoint.in_direction) {
            submit(endpoint, rhport);
        } else {
            sendOut(endpoint);
            if (!endpoint.out_pending) submitOut(endpoint, rhport);
        }
        if (hasPending(endpoint)) {
            ready_mask_.fetch_or(1u << contextIndex(endpoint.address),
                                 std::memory_order_release);
            if (metrics_ != nullptr) metrics_->add(MetricId::EndpointReady);
        }
    }
    const bool reschedule = ready_mask_.load(std::memory_order_acquire) != 0;
    unlock();
    if (reschedule) scheduleDrain();
}

bool EndpointRouter::mouseMove(int32_t x, int32_t y, int32_t wheel,
                               int32_t pan) {
    lock();
    if (policy_.mouseEndpointAddress() == 0 || !mouse_layout_.valid) {
        unlock();
        injection_moves_rejected_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const bool wheel_supported = mouse_layout_.wheel.present;
    const bool pan_supported = mouse_layout_.pan.present;
    const bool accepted = mouse_ledger_.add(
        x, y, wheel, pan, wheel_supported, pan_supported);
    unlock();
    if (!accepted) {
        injection_moves_rejected_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    injection_moves_accepted_.fetch_add(1, std::memory_order_relaxed);
    if (policy_.mouseEndpointAddress() != 0)
        markReady(policy_.mouseEndpointAddress());
    return true;
}

bool EndpointRouter::mouseButton(uint8_t mask, bool pressed) {
    lock();
    if (policy_.mouseEndpointAddress() == 0 || !mouse_layout_.valid) {
        unlock();
        injection_buttons_rejected_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const bool changed = mouse_ledger_.setButton(mask, pressed);
    unlock();
    if (!changed) return true;
    injection_buttons_accepted_.fetch_add(1, std::memory_order_relaxed);
    if (policy_.mouseEndpointAddress() != 0)
        markReady(policy_.mouseEndpointAddress());
    return true;
}

void EndpointRouter::setMouseLayout(const makcm::hid::MouseReportLayout &layout) {
    lock();
    mouse_layout_ = layout;
    unlock();
    mouse_layout_valid_.store(layout.valid, std::memory_order_release);
    mouse_layout_report_id_.store(layout.hasReportId ? layout.reportId : 0,
                                   std::memory_order_relaxed);
    mouse_layout_report_length_.store(layout.reportLength,
                                      std::memory_order_relaxed);
    if (policy_.mouseEndpointAddress() != 0)
        markReady(policy_.mouseEndpointAddress());
}

void EndpointRouter::resetMouseState() {
    lock();
    mouse_ledger_.reset();
    physical_fifo_head_ = 0;
    physical_fifo_tail_ = 0;
    physical_fifo_count_ = 0;
    std::memset(physical_fifo_lengths_, 0, sizeof(physical_fifo_lengths_));
    physical_generation_ = 0;
    unlock();
    if (policy_.mouseEndpointAddress() != 0)
        markReady(policy_.mouseEndpointAddress());
}

} // namespace makcm::native
