#pragma once

#include <array>
#include <atomic>
#include <stdint.h>

#include "BridgeProtocol.h"
#include "EndpointTxPhase.h"
#include "HidMouseReport.h"
#include "MouseInjectionLedger.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "bridge_transport.h"
#include "metrics.h"
#include "profile_store.h"
#include "proxy_policy.h"
#include "stick_runtime.h"

namespace makcm::native {

class EndpointRouter final {
public:
    static constexpr uint8_t kMaxEndpointNumber = 6;
    static constexpr uint8_t kQueueDepth = 16;
    static constexpr uint16_t kBufferSize = 64;

    EndpointRouter();
    ~EndpointRouter() = default;
    EndpointRouter(const EndpointRouter &) = delete;
    EndpointRouter &operator=(const EndpointRouter &) = delete;

    bool begin(ProfileStore *profile, BridgeTransport *bridge, uint32_t session,
               Metrics *metrics);
    void reset(uint32_t session);
    void setUsbReady(bool ready);
    void setStickRuntime(StickRuntime *runtime) { stick_runtime_ = runtime; }
    void bindStickPhysical(uint32_t session, uint64_t now_ms);
    void hotplugStick(uint64_t now_ms);
    bool handleStickLine(const char *line, uint64_t now_ms,
                         char *response, size_t response_capacity);
    bool takeStickResponse(char *response, size_t response_capacity);
    bool pollStickWatchdog(uint64_t now_ms);
    StickRuntimeSnapshot stickSnapshot(uint64_t now_ms);
    bool configureExactStickProfile(
        const char *format_id, const char *cal_id,
        const makcm::stick::ControllerProfile &allowed,
        const makcm::stick::ControllerProfileIdentity &observed);

    // Called by the RAW driver's open callback with the original descriptor.
    bool openEndpoint(uint8_t rhport, const uint8_t *descriptor,
                      uint16_t descriptor_length);
    void closeEndpoint(uint8_t rhport, uint8_t address);

    // Called by the bridge RX task.  It only updates mailboxes and schedules a
    // TinyUSB-context drain; it never calls a DCD private function directly.
    bool queueEndpointInput(uint8_t address, const uint8_t *data,
                            uint16_t length);

    // Called by the RAW xfer callback in TinyUSB context.
    void transferComplete(uint8_t rhport, uint8_t address, bool success,
                          uint32_t transferred);

    // Runs exclusively in TinyUSB task context through tinyusb::defer().
    void drain(uint8_t rhport);

    bool mouseMove(int32_t x, int32_t y, int32_t wheel = 0,
                   int32_t pan = 0);
    bool mouseButton(uint8_t mask, bool pressed);
    void setMouseLayout(const makcm::hid::MouseReportLayout &layout);
    void resetMouseState();

    uint32_t epoch() const { return epoch_.load(std::memory_order_acquire); }
    uint8_t mouseEndpointAddress() const {
        return policy_.mouseEndpointAddress();
    }
    uint32_t readyMask() const { return ready_mask_.load(); }
    const Metrics *metrics() const { return metrics_; }
    uint32_t openAttempts() const { return open_attempts_.load(); }
    uint32_t openFailures() const { return open_failures_.load(); }
    uint8_t lastOpenFailureStage() const {
        return last_open_failure_stage_.load();
    }
    uint8_t lastOpenFailureEndpoint() const {
        return last_open_failure_endpoint_.load();
    }
    bool allEndpointsOpen() const;
    uint32_t injectionMovesAccepted() const {
        return injection_moves_accepted_.load(std::memory_order_relaxed);
    }
    uint32_t injectionMovesRejected() const {
        return injection_moves_rejected_.load(std::memory_order_relaxed);
    }
    uint32_t injectionButtonsAccepted() const {
        return injection_buttons_accepted_.load(std::memory_order_relaxed);
    }
    uint32_t injectionButtonsRejected() const {
        return injection_buttons_rejected_.load(std::memory_order_relaxed);
    }
    bool mouseLayoutValid() const {
        return mouse_layout_valid_.load(std::memory_order_acquire);
    }
    uint8_t mouseLayoutReportId() const {
        return mouse_layout_report_id_.load(std::memory_order_relaxed);
    }
    uint16_t mouseLayoutReportLength() const {
        return mouse_layout_report_length_.load(std::memory_order_relaxed);
    }

private:
    enum class Source : uint8_t { None, Fifo, MousePhysical, MouseLedger, Out };

    struct EndpointContext {
        bool configured = false;
        bool in_direction = false;
        uint8_t address = 0;
        uint16_t max_packet = 0;
        uint8_t interval = 0;
        makcm::usb::EndpointTxPhase phase{};
        uint32_t epoch = 0;
        alignas(4) uint8_t compose[kBufferSize]{};
        alignas(4) uint8_t inflight[kBufferSize]{};
        uint16_t inflight_length = 0;
        Source source = Source::None;
        uint8_t source_slot = 0;
        uint32_t source_generation = 0;
        uint8_t fifo_head = 0;
        uint8_t fifo_tail = 0;
        uint8_t fifo_count = 0;
        uint16_t fifo_lengths[kQueueDepth]{};
        uint8_t fifo[kQueueDepth][kBufferSize]{};
        uint8_t out_buffer[kBufferSize]{};
        bool out_pending = false;
        uint16_t out_length = 0;
        // Metadata for the currently selected compose copy only. It never
        // changes FIFO selection/retirement and is cleared with Source.
        StickComposeResult stick_compose{};
    };

    static void deferredDrain(void *context);
    void scheduleDrain();
    EndpointContext *context(uint8_t address);
    const EndpointContext *context(uint8_t address) const;
    static uint8_t contextIndex(uint8_t address);
    void markReady(uint8_t address);
    bool chooseReport(EndpointContext &endpoint);
    bool chooseMouseReport(EndpointContext &endpoint);
    bool submit(EndpointContext &endpoint, uint8_t rhport);
    bool submitOut(EndpointContext &endpoint, uint8_t rhport);
    void finishSource(EndpointContext &endpoint, bool success);
    void sendOut(EndpointContext &endpoint);
    bool hasPending(const EndpointContext &endpoint) const;
    bool initializeMutex();
    void lock() const;
    void unlock() const;

    ProfileStore *profile_ = nullptr;
    BridgeTransport *bridge_ = nullptr;
    Metrics *metrics_ = nullptr;
    StickRuntime *stick_runtime_ = nullptr;
    uint32_t session_ = 0;
    std::atomic<uint32_t> epoch_{0};
    std::atomic<uint32_t> ready_mask_{0};
    std::atomic<bool> drain_scheduled_{false};
    std::atomic<bool> usb_ready_{false};
    std::atomic<uint32_t> open_attempts_{0};
    std::atomic<uint32_t> open_failures_{0};
    std::atomic<uint8_t> last_open_failure_stage_{0};
    std::atomic<uint8_t> last_open_failure_endpoint_{0};
    // Every caller that touches endpoint/runtime state runs in task context:
    // bridge RX, Secondary UART, or TinyUSB's deferred/xfer task.  A recursive
    // task mutex preserves the old serialization (including possible
    // synchronous TinyUSB completion re-entry) without disabling interrupts
    // across report composition and DCD submission.
    mutable StaticSemaphore_t mux_storage_{};
    mutable SemaphoreHandle_t mux_ = nullptr;
    std::array<EndpointContext, (kMaxEndpointNumber + 1) * 2> endpoints_{};
    ProxyPolicy policy_{};
    makcm::hid::MouseReportLayout mouse_layout_{};
    makcm::hid::MouseInjectionLedger mouse_ledger_{};
    // Physical mouse input is ordered data, not a state snapshot.  Keep a
    // bounded FIFO so a fast host report stream cannot overwrite an older
    // relative movement before the USB IN endpoint gets a chance to submit it.
    uint8_t physical_fifo_[kQueueDepth][kBufferSize]{};
    uint16_t physical_fifo_lengths_[kQueueDepth]{};
    uint32_t physical_fifo_generations_[kQueueDepth]{};
    uint8_t physical_fifo_head_ = 0;
    uint8_t physical_fifo_tail_ = 0;
    uint8_t physical_fifo_count_ = 0;
    uint32_t physical_generation_ = 0;
    std::atomic<uint32_t> injection_moves_accepted_{0};
    std::atomic<uint32_t> injection_moves_rejected_{0};
    std::atomic<uint32_t> injection_buttons_accepted_{0};
    std::atomic<uint32_t> injection_buttons_rejected_{0};
    std::atomic<bool> mouse_layout_valid_{false};
    std::atomic<uint8_t> mouse_layout_report_id_{0};
    std::atomic<uint16_t> mouse_layout_report_length_{0};
};

} // namespace makcm::native
