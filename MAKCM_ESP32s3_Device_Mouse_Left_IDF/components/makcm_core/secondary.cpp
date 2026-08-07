#include "secondary.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>

#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_timer.h"

namespace makcm::native {
namespace {
constexpr char kTag[] = "makcm-secondary";
constexpr uint8_t kSpeedSwitch[] = {0xDE, 0xAD, 0x05, 0x00, 0xA5,
                                    0x00, 0x09, 0x3D, 0x00};
constexpr uint8_t kButtonBits = 0x1F;

uint32_t nowMs() {
    return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
}

bool parsePair(const char *line, const char *prefix, int32_t &a, int32_t &b) {
    if (line == nullptr || prefix == nullptr ||
        std::strncmp(line, prefix, std::strlen(prefix)) != 0) return false;
    const char *p = line + std::strlen(prefix);
    while (*p == ' ' || *p == '(') ++p;
    char *end = nullptr;
    const long first = std::strtol(p, &end, 10);
    if (end == p) return false;
    while (*end == ' ' || *end == ',') ++end;
    char *end2 = nullptr;
    const long second = std::strtol(end, &end2, 10);
    if (end2 == end) return false;
    a = static_cast<int32_t>(std::clamp(first, static_cast<long>(INT32_MIN),
                                        static_cast<long>(INT32_MAX)));
    b = static_cast<int32_t>(std::clamp(second, static_cast<long>(INT32_MIN),
                                        static_cast<long>(INT32_MAX)));
    return true;
}

bool parseSingle(const char *line, const char *prefix, int32_t &value) {
    if (line == nullptr || prefix == nullptr ||
        std::strncmp(line, prefix, std::strlen(prefix)) != 0) return false;
    const char *p = line + std::strlen(prefix);
    while (*p == ' ' || *p == '(') ++p;
    char *end = nullptr;
    const long parsed = std::strtol(p, &end, 10);
    if (end == p) return false;
    value = static_cast<int32_t>(std::clamp(parsed, static_cast<long>(INT32_MIN),
                                            static_cast<long>(INT32_MAX)));
    return true;
}

bool parseButton(const char *line, const char *name, uint8_t bit, bool &pressed) {
    char expected[32];
    std::snprintf(expected, sizeof(expected), "km.%s(1)", name);
    if (std::strcmp(line, expected) == 0) {
        pressed = true;
        return true;
    }
    std::snprintf(expected, sizeof(expected), "km.%s(0)", name);
    if (std::strcmp(line, expected) == 0) {
        pressed = false;
        return true;
    }
    (void)bit;
    return false;
}

} // namespace

Secondary::~Secondary() { stop(); }

bool Secondary::begin(const SecondaryConfig &config, MoveHandler move_handler,
                      WheelHandler wheel_handler, ButtonHandler button_handler,
                      TextHandler text_handler, OtaFrameHandler ota_handler,
                      void *context) {
    if (running() || move_handler == nullptr || wheel_handler == nullptr ||
        button_handler == nullptr || config.initial_baud < 115200 ||
        config.fast_baud < config.initial_baud) return false;
    config_ = config;
    move_handler_ = move_handler;
    wheel_handler_ = wheel_handler;
    button_handler_ = button_handler;
    text_handler_ = text_handler;
    ota_handler_ = ota_handler;
    context_ = context;
    line_length_ = 0;
    ota_decoder_.reset();
    switch_index_ = 0;
    fast_baud_.store(false);
    requested_ota_maintenance_.store(false);
    ota_maintenance_.store(false);
    ota_mode_request_sequence_.store(0);
    ota_mode_applied_sequence_.store(0);
    physical_mask_.store(0);
    remote_mask_.store(0);
    commands_.store(0);
    parse_errors_.store(0);
    tx_errors_.store(0);
    rx_overflows_.store(0);
    minimum_free_task_stack_bytes_.store(kUnobservedStackHighWaterMark,
                                         std::memory_order_release);
    stream_enabled_ = false;
    stream_period_ms_ = 0;
    last_publish_ms_ = 0;

    const uart_config_t uart_config = {
        .baud_rate = config_.initial_baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
        .source_clk = UART_SCLK_DEFAULT,
#endif
    };
    if (uart_param_config(config_.port, &uart_config) != ESP_OK ||
        uart_set_pin(config_.port, config_.tx_pin, config_.rx_pin,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK ||
        uart_driver_install(config_.port, config_.rx_buffer_size,
                            config_.tx_buffer_size, 0, nullptr, 0) != ESP_OK) {
        return false;
    }
    tx_mutex_ = xSemaphoreCreateMutex();
    if (tx_mutex_ == nullptr) {
        uart_driver_delete(config_.port);
        return false;
    }
    running_.store(true, std::memory_order_release);
    if (xTaskCreatePinnedToCore(&Secondary::taskThunk, "makcm_secondary",
                                config_.task_stack, this, config_.task_priority,
                                &task_, config_.task_core) != pdPASS) {
        running_.store(false);
        vSemaphoreDelete(tx_mutex_);
        tx_mutex_ = nullptr;
        uart_driver_delete(config_.port);
        return false;
    }
    ESP_LOGI(kTag, "UART%u initial=%d fast=%d", config_.port,
             config_.initial_baud, config_.fast_baud);
    return true;
}

void Secondary::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) return;
    if (task_ != nullptr) {
        vTaskDelete(task_);
        task_ = nullptr;
    }
    uart_driver_delete(config_.port);
    if (tx_mutex_ != nullptr) {
        vSemaphoreDelete(tx_mutex_);
        tx_mutex_ = nullptr;
    }
    context_ = nullptr;
}

bool Secondary::sendText(const char *text) {
    // Answers to text commands are the diagnostic plane, not the data plane --
    // every caller is a control/status reply (LEFT_STATUS, LEFT_CFG, OK,
    // RIGHT: relays).  Mouse reports leave through sendButtonFrame()/the stream
    // path, which stay gated on maintenance.  Receiving a command but silently
    // dropping its answer is the same black box as not parsing it at all.
    if (text == nullptr) return false;
    const size_t length = std::strlen(text);
    if (length == 0 || length > kMaxTextResponseLength) return false;
    return writeBytes(reinterpret_cast<const uint8_t *>(text), length);
}

bool Secondary::sendOtaFrame(const makcm::ota::Frame &frame) {
    if (!running() || tx_mutex_ == nullptr ||
        xSemaphoreTake(tx_mutex_, pdMS_TO_TICKS(100)) != pdTRUE) {
        tx_errors_.fetch_add(1);
        return false;
    }
    const size_t length = makcm::ota::encodeFrame(
        frame, ota_tx_wire_, sizeof(ota_tx_wire_));
    const int written = length == 0
        ? -1
        : uart_write_bytes(config_.port,
                           reinterpret_cast<const char *>(ota_tx_wire_),
                           length);
    xSemaphoreGive(tx_mutex_);
    if (length == 0 || written != static_cast<int>(length)) {
        tx_errors_.fetch_add(1);
        return false;
    }
    return true;
}

bool Secondary::sendButtonFrame(uint8_t mask) {
    if (otaMaintenance()) return false;
    const uint8_t frame[] = {'k', 'm', '.', mask, '\r', '\n', '>', '>', '>', ' '};
    return writeBytes(frame, sizeof(frame));
}

void Secondary::setPhysicalMask(uint8_t mask) {
    const uint8_t old = physical_mask_.exchange(mask & kButtonBits);
    if (old != (mask & kButtonBits) && stream_enabled_) publishButtonState(true);
}

bool Secondary::writeBytes(const uint8_t *data, size_t length) {
    if (!running() || data == nullptr || length == 0 || tx_mutex_ == nullptr ||
        xSemaphoreTake(tx_mutex_, pdMS_TO_TICKS(20)) != pdTRUE) {
        tx_errors_.fetch_add(1);
        return false;
    }
    const int written = uart_write_bytes(config_.port,
                                         reinterpret_cast<const char *>(data),
                                         length);
    xSemaphoreGive(tx_mutex_);
    if (written != static_cast<int>(length)) {
        tx_errors_.fetch_add(1);
        return false;
    }
    return true;
}

void Secondary::taskThunk(void *arg) {
    static_cast<Secondary *>(arg)->task();
    vTaskDelete(nullptr);
}

void Secondary::task() {
    uint8_t buffer[256];
    updateMinimumFreeTaskStackBytes();
    while (running()) {
        applyPendingOtaMode();
        // The project runs FreeRTOS at 100 Hz, so pdMS_TO_TICKS(2) rounds
        // down to zero and turns uart_read_bytes() into a busy poll.  That
        // starves IDLE1 while the CH343 stream is active and trips the task
        // watchdog.  Use a real tick-sized wait; the control UART does not
        // need sub-tick latency.
        const int count = uart_read_bytes(config_.port, buffer, sizeof(buffer),
                                          pdMS_TO_TICKS(10));
        if (count > 0) {
            for (int i = 0; i < count; ++i) handleByte(buffer[i]);
            updateMinimumFreeTaskStackBytes();
            // Bound CPU residency if bytes are continuously available.
            vTaskDelay(1);
        }
        if (stream_enabled_ && stream_period_ms_ != 0) {
            const uint32_t now = nowMs();
            if (last_publish_ms_ == 0 || now - last_publish_ms_ >= stream_period_ms_)
                publishButtonState(false);
        }
    }
}

bool Secondary::setOtaMaintenance(bool enabled) {
    if (!running()) return false;
    requested_ota_maintenance_.store(enabled, std::memory_order_release);
    const uint32_t request_sequence =
        ota_mode_request_sequence_.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (xTaskGetCurrentTaskHandle() == task_) applyPendingOtaMode();
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(100);
    while (running() &&
           ota_mode_applied_sequence_.load(std::memory_order_acquire) !=
               request_sequence) {
        if (static_cast<int32_t>(xTaskGetTickCount() - deadline) >= 0)
            return false;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return running() &&
           ota_mode_applied_sequence_.load(std::memory_order_acquire) ==
               request_sequence;
}

void Secondary::applyPendingOtaMode() {
    const uint32_t requested =
        ota_mode_request_sequence_.load(std::memory_order_acquire);
    if (requested ==
        ota_mode_applied_sequence_.load(std::memory_order_acquire))
        return;
    ota_decoder_.reset();
    line_length_ = 0;
    switch_index_ = 0;
    ota_maintenance_.store(
        requested_ota_maintenance_.load(std::memory_order_acquire),
        std::memory_order_release);
    ota_mode_applied_sequence_.store(requested, std::memory_order_release);
}

void Secondary::updateMinimumFreeTaskStackBytes() {
    // FreeRTOS reports this value in StackType_t units.  The ESP32-S3
    // Xtensa port uses a byte-sized StackType_t, so this is already a byte
    // count for this IDF target; multiplying by sizeof(StackType_t) keeps the
    // conversion explicit if the port's stack element width ever changes.
    const uint32_t free_stack_bytes =
        static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr)) *
        static_cast<uint32_t>(sizeof(StackType_t));
    uint32_t minimum =
        minimum_free_task_stack_bytes_.load(std::memory_order_relaxed);
    while (free_stack_bytes < minimum &&
           !minimum_free_task_stack_bytes_.compare_exchange_weak(
               minimum, free_stack_bytes, std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
}

void Secondary::handleByte(uint8_t byte) {
    applyPendingOtaMode();
    // MKOT shares CH343 with the text control plane.  Once its exact magic is
    // seen at a text-frame boundary, every following byte belongs to the
    // bounded CRC-protected decoder — including on an Idle board that has not
    // yet armed exclusive maintenance.  Left HELLO / ENTER_MAINTENANCE are the
    // bootstrap that calls otaQuiesce(); gating the decoder on
    // ota_maintenance_ made Idle discovery time out forever while km.* text
    // still worked.
    if (ota_decoder_.buffered() != 0 ||
        (line_length_ == 0 && byte == makcm::ota::kMagic[0])) {
        const auto result = ota_decoder_.feed(byte, ota_rx_frame_);
        if (result == makcm::ota::FeedResult::FrameReady) {
            if (ota_handler_ != nullptr) {
                ota_handler_(context_, ota_rx_frame_);
            } else {
                parse_errors_.fetch_add(1);
            }
        } else if (result == makcm::ota::FeedResult::FrameError) {
            parse_errors_.fetch_add(1);
        }
        return;
    }
    // The baud switch is part of the OTA recovery transport as well as the
    // legacy command transport.  Keep it active while maintenance is
    // exclusive; otherwise a reboot into pending-confirm/fault maintenance at
    // 115200 baud cannot be reached by the 4 M Linux USB2 updater.
    if (byte == kSpeedSwitch[switch_index_]) {
        ++switch_index_;
        if (switch_index_ == sizeof(kSpeedSwitch)) {
            switch_index_ = 0;
            uart_set_baudrate(config_.port, config_.fast_baud);
            fast_baud_.store(true, std::memory_order_release);
        }
        return;
    }
    switch_index_ = byte == kSpeedSwitch[0] ? 1 : 0;
    // Deliberately NOT gated on ota_maintenance_.  The maintenance gate owns
    // the data plane; killing the diagnostic plane with it turned every failed
    // update into a black box -- LEFT_STATUS went silent exactly when its
    // counters were the only way to tell "the peer never answered" from "we ate
    // the answer", and a quiesce that would not release had no observable
    // cause at all.  MKOT and text already coexist safely: the decoder above
    // claims the stream only from the exact magic at a text-frame boundary, so
    // a text line can never be mistaken for a frame.
    //
    // The standing contract this relies on: on CH343, a leading `M` belongs to
    // MKOT.  Every text command is safe today (km.* is lower case; LEFT_*,
    // READY, DEBUG_*, OTA_* do not start with M) and must stay that way.
    if (byte == '\r') return;
    if (byte == '\n') {
        handleLine();
        line_length_ = 0;
        return;
    }
    if (line_length_ + 1 >= sizeof(line_)) {
        line_length_ = 0;
        rx_overflows_.fetch_add(1);
        return;
    }
    line_[line_length_++] = static_cast<char>(byte);
    line_[line_length_] = '\0';
}

void Secondary::handleLine() {
    while (line_length_ != 0 && std::isspace(static_cast<unsigned char>(line_[line_length_ - 1])))
        line_[--line_length_] = '\0';
    size_t start = 0;
    while (std::isspace(static_cast<unsigned char>(line_[start]))) ++start;
    if (start != 0) {
        std::memmove(line_, line_ + start, line_length_ - start + 1);
        line_length_ -= start;
    }
    if (line_length_ == 0) return;
    ++commands_;
    int32_t x = 0;
    int32_t y = 0;
    if (parsePair(line_, "km.move", x, y)) {
        move_handler_(context_, x, y);
        return;
    }
    int32_t wheel = 0;
    if (parseSingle(line_, "km.wheel", wheel)) {
        wheel_handler_(context_, wheel);
        return;
    }
    bool pressed = false;
    const struct {
        const char *name;
        uint8_t bit;
    } buttons[] = {{"left", 1u << 0}, {"right", 1u << 1},
                   {"middle", 1u << 2}, {"side1", 1u << 3},
                   {"side2", 1u << 4}};
    for (const auto &button : buttons) {
        if (parseButton(line_, button.name, button.bit, pressed)) {
            uint8_t old = remote_mask_.load();
            const uint8_t next = pressed ? static_cast<uint8_t>(old | button.bit)
                                         : static_cast<uint8_t>(old & ~button.bit);
            remote_mask_.store(next);
            button_handler_(context_, button.bit, pressed);
            return;
        }
    }
    if (std::strncmp(line_, "km.buttons", 10) == 0) {
        int enabled = 0;
        int period = 0;
        if (std::sscanf(line_, "km.buttons(%d,%d)", &enabled, &period) >= 1) {
            stream_enabled_ = enabled != 0;
            stream_period_ms_ = period > 0 ? static_cast<uint32_t>(
                std::clamp(period, 1, 1000)) : 0;
            publishButtonState(true);
            return;
        }
    }
    if (text_handler_ != nullptr) {
        text_handler_(context_, line_);
    } else {
        parse_errors_.fetch_add(1);
    }
}

void Secondary::publishButtonState(bool force) {
    if (!stream_enabled_ && !force) return;
    const uint8_t mask = static_cast<uint8_t>(physical_mask_.load() |
                                              remote_mask_.load());
    if (sendButtonFrame(mask)) last_publish_ms_ = nowMs();
}

} // namespace makcm::native
