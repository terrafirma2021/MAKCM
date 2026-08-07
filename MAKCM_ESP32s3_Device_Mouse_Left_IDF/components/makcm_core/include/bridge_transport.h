#pragma once

#include <atomic>
#include <stdint.h>

#include "BridgeProtocol.h"
#include "BridgeSequenceTracker.h"
#include "OtaProtocol.h"

#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace makcm::native {

struct BridgeTransportConfig {
    uart_port_t port = UART_NUM_1;
    int rx_pin = 1;
    int tx_pin = 2;
    int baud = 5000000;
    int rx_buffer_size = 8192;
    int tx_buffer_size = 4096;
    int event_queue_size = 32;
    int tx_queue_size = 32;
    uint8_t task_core = 1;
    uint8_t task_priority = 18;
    uint16_t task_stack = 6144;
};

class BridgeTransport final {
public:
    using FrameHandler = void (*)(void *context,
                                  const makcm::bridge::Frame &frame);
    using TextHandler = void (*)(void *context, uint8_t byte);
    using OtaFrameHandler = void (*)(void *context,
                                     const makcm::ota::Frame &frame);

    BridgeTransport() = default;
    ~BridgeTransport();
    BridgeTransport(const BridgeTransport &) = delete;
    BridgeTransport &operator=(const BridgeTransport &) = delete;

    bool begin(const BridgeTransportConfig &config, FrameHandler frame_handler,
               TextHandler text_handler = nullptr,
               OtaFrameHandler ota_frame_handler = nullptr,
               void *context = nullptr);
    void stop();

    bool send(makcm::bridge::MessageType type, const uint8_t *payload,
              uint16_t length, uint32_t session);
    // Forward an ASCII line over the mixed text/binary UART stream.  Text
    // commands are consumed by the right Arduino receiver; binary protocol
    // frames must continue to use send() so the COBS decoder stays intact.
    bool sendText(const char *text);
    // OTA control text (OTA_MAINT_BEGIN).  Deliberately separate from
    // sendText(): that one is the diagnostic relay and stays gated on
    // maintenance, while this one is how a NEW transaction bootstraps the peer
    // and therefore must work while maintenance is still held for an older one.
    bool sendOtaText(const char *text);
    bool sendOta(const makcm::ota::Frame &frame);
    bool setOtaMaintenance(bool enabled);
    bool otaMaintenance() const {
        return requested_ota_maintenance_.load(std::memory_order_acquire);
    }
    bool sendEmpty(makcm::bridge::MessageType type, uint32_t session) {
        return send(type, nullptr, 0, session);
    }

    bool running() const { return running_.load(std::memory_order_acquire); }
    uint16_t nextSequence();
    uint32_t rxFrames() const { return rx_frames_.load(); }
    uint32_t rxErrors() const { return rx_errors_.load(); }
    uint32_t txErrors() const { return tx_errors_.load(); }
    uint32_t textBytes() const { return text_bytes_.load(); }
    // Minimum free stack high-water mark in bytes for the makcm_bridge_rx
    // task; 0 means the task has not been observed yet.  This task decodes
    // right-board MKOT responses and runs the pair-persistence path inline, so
    // it is the one that actually starves.  Without this number LEFT_STATUS
    // reported only the Secondary task's headroom, which stayed flat and
    // healthy while makcm_bridge_rx was overflowing its stack.
    uint32_t minimumFreeRxTaskStackBytes() const {
        const uint32_t value =
            minimum_free_rx_task_stack_bytes_.load(std::memory_order_acquire);
        return value == kUnobservedStackHighWaterMark ? 0u : value;
    }
    uint32_t rxInputSequenceFrames() const { return input_sequence_.frames(); }
    uint32_t rxInputSequenceGapFrames() const {
        return input_sequence_.gapFrames();
    }
    uint32_t rxInputSequenceGapEvents() const {
        return input_sequence_.gapEvents();
    }
    uint32_t rxInputSequenceDuplicates() const {
        return input_sequence_.duplicates();
    }
    uint32_t rxInputSequenceOutOfOrder() const {
        return input_sequence_.outOfOrder();
    }
    uint32_t rxInputSequenceSessionChanges() const {
        return input_sequence_.sessionChanges();
    }

private:
    struct TxItem {
        uint16_t length = 0;
        uint8_t wire[makcm::bridge::kMaxWireFrameSize]{};
    };

    static constexpr uint32_t kUnobservedStackHighWaterMark = UINT32_MAX;

    static void rxTaskThunk(void *arg);
    static void txTaskThunk(void *arg);
    void rxTask();
    void txTask();
    void recordRxTaskStackHighWaterMark();
    void handleByte(uint8_t byte);
    void applyPendingOtaMode();
    bool requestOtaModeApply();
    bool otaModeChangePending() const {
        return ota_mode_request_sequence_.load(std::memory_order_acquire) !=
               ota_mode_applied_sequence_.load(std::memory_order_acquire);
    }
    bool otaRxArmed() const {
        return ota_maintenance_.load(std::memory_order_acquire);
    }

    BridgeTransportConfig config_{};
    FrameHandler frame_handler_ = nullptr;
    TextHandler text_handler_ = nullptr;
    OtaFrameHandler ota_frame_handler_ = nullptr;
    void *context_ = nullptr;
    QueueHandle_t event_queue_ = nullptr;
    QueueHandle_t tx_queue_ = nullptr;
    SemaphoreHandle_t tx_mutex_ = nullptr;
    TaskHandle_t task_ = nullptr;
    TaskHandle_t tx_task_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<bool> requested_ota_maintenance_{false};
    std::atomic<bool> ota_maintenance_{false};
    std::atomic<uint32_t> ota_mode_request_sequence_{0};
    std::atomic<uint32_t> ota_mode_applied_sequence_{0};
    std::atomic<uint16_t> tx_sequence_{0};
    std::atomic<uint32_t> rx_frames_{0};
    std::atomic<uint32_t> rx_errors_{0};
    std::atomic<uint32_t> tx_errors_{0};
    std::atomic<uint32_t> text_bytes_{0};
    std::atomic<uint32_t> minimum_free_rx_task_stack_bytes_{
        kUnobservedStackHighWaterMark};
    // Text-frame boundary tracker for the OTA-maintenance RX path.  Starts true
    // so the first byte after entering maintenance can arm the MKOT decoder.
    bool ota_line_idle_ = true;
    makcm::bridge::InputSequenceTracker input_sequence_{};
    makcm::bridge::MixedStreamDecoder decoder_{};
    makcm::ota::StreamDecoder ota_decoder_{};
    makcm::ota::Frame ota_rx_frame_{};
    uint8_t ota_tx_wire_[makcm::ota::kMaxWireSize]{};
};

} // namespace makcm::native
