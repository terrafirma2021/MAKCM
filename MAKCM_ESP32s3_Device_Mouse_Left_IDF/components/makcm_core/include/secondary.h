#pragma once

#include <atomic>
#include <stdint.h>

#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "OtaProtocol.h"

namespace makcm::native {

struct SecondaryConfig {
    uart_port_t port = UART_NUM_0;
    int rx_pin = UART_PIN_NO_CHANGE;
    int tx_pin = UART_PIN_NO_CHANGE;
    int initial_baud = 115200;
    int fast_baud = 4000000;
    // CH343 carries MKOT, so a full DATA frame (kMaxWireSize = 4124 B) must fit
    // in the driver ring with room to spare.  A ring smaller than one frame
    // makes reception depend on the RX task being scheduled mid-frame rather
    // than being structurally guaranteed; a single dropped byte fails CRC, the
    // board answers nothing, and the host can only report a timeout.
    int rx_buffer_size = 2 * static_cast<int>(makcm::ota::kMaxWireSize);
    int tx_buffer_size = 2048;
    uint8_t task_core = 1;
    uint8_t task_priority = 12;
    uint16_t task_stack = 4096;
};

class Secondary final {
public:
    static constexpr size_t kMaxTextResponseLength = 1023;
    using MoveHandler = void (*)(void *context, int32_t x, int32_t y);
    using WheelHandler = void (*)(void *context, int32_t wheel);
    using ButtonHandler = void (*)(void *context, uint8_t mask, bool pressed);
    using TextHandler = void (*)(void *context, const char *line);
    using OtaFrameHandler = void (*)(void *context,
                                     const makcm::ota::Frame &frame);

    Secondary() = default;
    ~Secondary();
    Secondary(const Secondary &) = delete;
    Secondary &operator=(const Secondary &) = delete;

    bool begin(const SecondaryConfig &config, MoveHandler move_handler,
               WheelHandler wheel_handler, ButtonHandler button_handler,
               TextHandler text_handler = nullptr,
               OtaFrameHandler ota_handler = nullptr,
               void *context = nullptr);
    void stop();

    bool sendText(const char *text);
    bool sendOtaFrame(const makcm::ota::Frame &frame);
    bool setOtaMaintenance(bool enabled);
    bool otaMaintenance() const {
        return requested_ota_maintenance_.load(std::memory_order_acquire);
    }
    bool sendButtonFrame(uint8_t mask);
    void setPhysicalMask(uint8_t mask);
    bool running() const { return running_.load(std::memory_order_acquire); }
    uint8_t physicalMask() const { return physical_mask_.load(); }

    uint32_t commands() const { return commands_.load(); }
    uint32_t parseErrors() const { return parse_errors_.load(); }
    uint32_t txErrors() const { return tx_errors_.load(); }
    uint32_t rxOverflows() const { return rx_overflows_.load(); }
    // Returns the minimum free stack high-water mark in bytes observed for
    // the Secondary task.  Zero means that the task has not been observed.
    uint32_t minimumFreeTaskStackBytes() const {
        const uint32_t value =
            minimum_free_task_stack_bytes_.load(std::memory_order_acquire);
        return value == kUnobservedStackHighWaterMark ? 0u : value;
    }
    bool fastBaud() const { return fast_baud_.load(); }

private:
    static constexpr uint32_t kUnobservedStackHighWaterMark = UINT32_MAX;

    static void taskThunk(void *arg);
    void task();
    void updateMinimumFreeTaskStackBytes();
    void handleByte(uint8_t byte);
    void applyPendingOtaMode();
    void handleLine();
    void publishButtonState(bool force);
    bool writeBytes(const uint8_t *data, size_t length);

    SecondaryConfig config_{};
    MoveHandler move_handler_ = nullptr;
    WheelHandler wheel_handler_ = nullptr;
    ButtonHandler button_handler_ = nullptr;
    TextHandler text_handler_ = nullptr;
    OtaFrameHandler ota_handler_ = nullptr;
    void *context_ = nullptr;
    SemaphoreHandle_t tx_mutex_ = nullptr;
    TaskHandle_t task_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<bool> fast_baud_{false};
    std::atomic<bool> requested_ota_maintenance_{false};
    std::atomic<bool> ota_maintenance_{false};
    std::atomic<uint32_t> ota_mode_request_sequence_{0};
    std::atomic<uint32_t> ota_mode_applied_sequence_{0};
    std::atomic<uint8_t> physical_mask_{0};
    std::atomic<uint8_t> remote_mask_{0};
    std::atomic<uint32_t> commands_{0};
    std::atomic<uint32_t> parse_errors_{0};
    std::atomic<uint32_t> tx_errors_{0};
    std::atomic<uint32_t> rx_overflows_{0};
    std::atomic<uint32_t> minimum_free_task_stack_bytes_{
        kUnobservedStackHighWaterMark};
    bool stream_enabled_ = false;
    uint32_t stream_period_ms_ = 0;
    uint32_t last_publish_ms_ = 0;
    uint8_t switch_index_ = 0;
    char line_[640] = {};
    size_t line_length_ = 0;
    makcm::ota::StreamDecoder ota_decoder_{};
    makcm::ota::Frame ota_rx_frame_{};
    uint8_t ota_tx_wire_[makcm::ota::kMaxWireSize]{};
};

} // namespace makcm::native
