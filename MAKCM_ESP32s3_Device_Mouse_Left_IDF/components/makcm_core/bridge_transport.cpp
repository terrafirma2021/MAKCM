#include "bridge_transport.h"

#include <algorithm>
#include <cstring>

#include "esp_check.h"
#include "esp_idf_version.h"
#include "esp_log.h"

namespace makcm::native {
namespace {
constexpr char kTag[] = "makcm-bridge";
}

BridgeTransport::~BridgeTransport() { stop(); }

bool BridgeTransport::begin(const BridgeTransportConfig &config,
                            FrameHandler frame_handler,
                            TextHandler text_handler,
                            OtaFrameHandler ota_frame_handler,
                            void *context) {
    if (running()) return false;
    if (frame_handler == nullptr || config.rx_buffer_size < 512 ||
        config.tx_buffer_size < 256 || config.baud < 115200) {
        return false;
    }

    config_ = config;
    frame_handler_ = frame_handler;
    text_handler_ = text_handler;
    ota_frame_handler_ = ota_frame_handler;
    context_ = context;
    decoder_.reset();
    ota_decoder_.reset();
    requested_ota_maintenance_.store(false);
    ota_maintenance_.store(false);
    ota_mode_request_sequence_.store(0);
    ota_mode_applied_sequence_.store(0);
    tx_sequence_.store(0);
    rx_frames_.store(0);
    rx_errors_.store(0);
    tx_errors_.store(0);
    text_bytes_.store(0);
    input_sequence_.reset();

    const uart_config_t uart_config = {
        .baud_rate = config_.baud,
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
                            config_.tx_buffer_size, config_.event_queue_size,
                            &event_queue_, 0) != ESP_OK) {
        event_queue_ = nullptr;
        return false;
    }

    tx_queue_ = xQueueCreate(config_.tx_queue_size, sizeof(TxItem));
    tx_mutex_ = xSemaphoreCreateMutex();
    if (tx_queue_ == nullptr || tx_mutex_ == nullptr) {
        if (tx_queue_ != nullptr) vQueueDelete(tx_queue_);
        tx_queue_ = nullptr;
        if (tx_mutex_ != nullptr) vSemaphoreDelete(tx_mutex_);
        tx_mutex_ = nullptr;
        uart_driver_delete(config_.port);
        event_queue_ = nullptr;
        return false;
    }

    running_.store(true, std::memory_order_release);
    const BaseType_t created = xTaskCreatePinnedToCore(
        &BridgeTransport::rxTaskThunk, "makcm_bridge_rx", config_.task_stack,
        this, config_.task_priority, &task_, config_.task_core);
    if (created != pdPASS) {
        running_.store(false, std::memory_order_release);
        vQueueDelete(tx_queue_);
        tx_queue_ = nullptr;
        vSemaphoreDelete(tx_mutex_);
        tx_mutex_ = nullptr;
        uart_driver_delete(config_.port);
        event_queue_ = nullptr;
        return false;
    }
    const BaseType_t tx_created = xTaskCreatePinnedToCore(
        &BridgeTransport::txTaskThunk, "makcm_bridge_tx", config_.task_stack,
        this, config_.task_priority, &tx_task_, config_.task_core);
    if (tx_created != pdPASS) {
        running_.store(false, std::memory_order_release);
        vTaskDelete(task_);
        task_ = nullptr;
        vQueueDelete(tx_queue_);
        tx_queue_ = nullptr;
        vSemaphoreDelete(tx_mutex_);
        tx_mutex_ = nullptr;
        uart_driver_delete(config_.port);
        event_queue_ = nullptr;
        return false;
    }
    ESP_LOGI(kTag, "UART%u RX=%d TX=%d baud=%d", config_.port,
             config_.rx_pin, config_.tx_pin, config_.baud);
    return true;
}

void BridgeTransport::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) return;

    if (task_ != nullptr) {
        // The task observes running_ between UART events.  Delete it here so
        // the UART driver cannot outlive the task's event queue.
        vTaskDelete(task_);
        task_ = nullptr;
    }
    if (tx_task_ != nullptr) {
        vTaskDelete(tx_task_);
        tx_task_ = nullptr;
    }
    if (event_queue_ != nullptr) {
        uart_driver_delete(config_.port);
        event_queue_ = nullptr;
    }
    if (tx_queue_ != nullptr) {
        vQueueDelete(tx_queue_);
        tx_queue_ = nullptr;
    }
    if (tx_mutex_ != nullptr) {
        vSemaphoreDelete(tx_mutex_);
        tx_mutex_ = nullptr;
    }
    frame_handler_ = nullptr;
    text_handler_ = nullptr;
    ota_frame_handler_ = nullptr;
    context_ = nullptr;
}

uint16_t BridgeTransport::nextSequence() {
    return tx_sequence_.fetch_add(1, std::memory_order_relaxed);
}

bool BridgeTransport::send(makcm::bridge::MessageType type,
                           const uint8_t *payload, uint16_t length,
                           uint32_t session) {
    if (!running() || otaMaintenance() || tx_queue_ == nullptr || length >
            makcm::bridge::kMaxPayloadSize) {
        tx_errors_.fetch_add(1);
        return false;
    }

    TxItem item{};
    const size_t wire_length = makcm::bridge::encodeFrame(
        type, nextSequence(), payload, length, item.wire, sizeof(item.wire),
        session);
    if (wire_length == 0 || wire_length > UINT16_MAX) {
        tx_errors_.fetch_add(1);
        return false;
    }
    item.length = static_cast<uint16_t>(wire_length);
    if (xQueueSend(tx_queue_, &item, 0) != pdTRUE) {
        tx_errors_.fetch_add(1);
        return false;
    }
    return true;
}

bool BridgeTransport::sendText(const char *text) {
    if (!running() || otaMaintenance() || tx_queue_ == nullptr || text == nullptr) {
        tx_errors_.fetch_add(1);
        return false;
    }
    const size_t length = std::strlen(text);
    if (length == 0 || length > sizeof(TxItem::wire)) {
        tx_errors_.fetch_add(1);
        return false;
    }

    TxItem item{};
    std::memcpy(item.wire, text, length);
    item.length = static_cast<uint16_t>(length);
    if (xQueueSend(tx_queue_, &item, 0) != pdTRUE) {
        tx_errors_.fetch_add(1);
        return false;
    }
    return true;
}

bool BridgeTransport::sendOtaText(const char *text) {
    // OTA_MAINT_BEGIN is how a new transaction bootstraps the right board into
    // maintenance.  Gating it the way sendText() is gated deadlocks the pair:
    // startRightMaintenanceBootstrap() is reached precisely when maintenance is
    // held for a *different* transaction, so the one message that can retarget
    // the link was suppressed by the very latch it exists to clear.  The host
    // saw only "timed out", because the resulting compact command-response
    // Busy NACK did not complete the requested right-board bootstrap.
    //
    // Written synchronously under the tx mutex instead of through tx_queue_,
    // because txTask() discards queued items while maintenance is held.  The
    // mutex keeps a whole line from interleaving into an MKOT frame, and the
    // right board already accepts this exact line while latched in maintenance.
    if (!running() || tx_mutex_ == nullptr || text == nullptr) {
        tx_errors_.fetch_add(1);
        return false;
    }
    const size_t length = std::strlen(text);
    if (length == 0 || length > sizeof(TxItem::wire)) {
        tx_errors_.fetch_add(1);
        return false;
    }
    if (xSemaphoreTake(tx_mutex_, pdMS_TO_TICKS(250)) != pdTRUE) {
        tx_errors_.fetch_add(1);
        return false;
    }
    const int written = uart_write_bytes(config_.port, text, length);
    xSemaphoreGive(tx_mutex_);
    if (written != static_cast<int>(length)) {
        tx_errors_.fetch_add(1);
        return false;
    }
    return true;
}

bool BridgeTransport::requestOtaModeApply() {
    if (!running() || tx_queue_ == nullptr || tx_mutex_ == nullptr ||
        xSemaphoreTake(tx_mutex_, pdMS_TO_TICKS(100)) != pdTRUE) return false;
    // Parser objects are owned exclusively by the RX task.  Publish a mode
    // request here and let that task reset/apply it at a byte boundary; a
    // direct reset from app_main races MixedStreamDecoder::feed().
    xQueueReset(tx_queue_);
    const uint32_t request_sequence =
        ota_mode_request_sequence_.fetch_add(1, std::memory_order_acq_rel) + 1;
    xSemaphoreGive(tx_mutex_);
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

bool BridgeTransport::setOtaMaintenance(bool enabled) {
    requested_ota_maintenance_.store(enabled, std::memory_order_release);
    return requestOtaModeApply();
}

bool BridgeTransport::sendOta(const makcm::ota::Frame &frame) {
    if (!running() || !otaMaintenance() || tx_mutex_ == nullptr ||
        xSemaphoreTake(tx_mutex_, pdMS_TO_TICKS(250)) != pdTRUE) {
        tx_errors_.fetch_add(1);
        return false;
    }
    const size_t length = makcm::ota::encodeFrame(
        frame, ota_tx_wire_, sizeof(ota_tx_wire_));
    const int written = length == 0 ? -1 : uart_write_bytes(
        config_.port, reinterpret_cast<const char *>(ota_tx_wire_), length);
    xSemaphoreGive(tx_mutex_);
    if (length == 0 || written != static_cast<int>(length)) {
        tx_errors_.fetch_add(1);
        return false;
    }
    return true;
}

void BridgeTransport::recordRxTaskStackHighWaterMark() {
    // FreeRTOS reports this value in StackType_t units.  The ESP32-S3 Xtensa
    // port uses a byte-sized StackType_t, so this is already a byte count for
    // this IDF target; multiplying by sizeof(StackType_t) keeps the conversion
    // explicit if the port's stack element width ever changes.
    const uint32_t free_stack_bytes =
        static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr)) *
        static_cast<uint32_t>(sizeof(StackType_t));
    uint32_t minimum =
        minimum_free_rx_task_stack_bytes_.load(std::memory_order_relaxed);
    while (free_stack_bytes < minimum &&
           !minimum_free_rx_task_stack_bytes_.compare_exchange_weak(
               minimum, free_stack_bytes, std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
}

void BridgeTransport::rxTaskThunk(void *arg) {
    static_cast<BridgeTransport *>(arg)->rxTask();
    vTaskDelete(nullptr);
}

void BridgeTransport::txTaskThunk(void *arg) {
    static_cast<BridgeTransport *>(arg)->txTask();
    vTaskDelete(nullptr);
}

void BridgeTransport::txTask() {
    TxItem item{};
    while (running()) {
        if (xQueueReceive(tx_queue_, &item, pdMS_TO_TICKS(20)) != pdTRUE)
            continue;
        if (otaMaintenance() || otaModeChangePending()) continue;
        if (xSemaphoreTake(tx_mutex_, pdMS_TO_TICKS(100)) != pdTRUE) {
            tx_errors_.fetch_add(1);
            continue;
        }
        if (otaMaintenance() || otaModeChangePending()) {
            xSemaphoreGive(tx_mutex_);
            continue;
        }
        const int written = uart_write_bytes(
            config_.port, reinterpret_cast<const char *>(item.wire), item.length);
        xSemaphoreGive(tx_mutex_);
        if (written != item.length) tx_errors_.fetch_add(1);
    }
}

void BridgeTransport::rxTask() {
    uart_event_t event{};
    uint8_t buffer[256];
    while (running()) {
        applyPendingOtaMode();
        if (xQueueReceive(event_queue_, &event, pdMS_TO_TICKS(20)) != pdTRUE)
            continue;
        if (event.type == UART_DATA) {
            size_t remaining = event.size;
            while (remaining != 0) {
                const size_t want = std::min(remaining, sizeof(buffer));
                const int count = uart_read_bytes(config_.port, buffer, want,
                                                 pdMS_TO_TICKS(20));
                if (count <= 0) break;
                remaining -= static_cast<size_t>(count);
                for (int i = 0; i < count; ++i) handleByte(buffer[i]);
                // Sampled after the batch so it covers the deepest frame the
                // batch delivered -- an MKOT response unwinds the whole
                // pair-persistence path before returning here.
                recordRxTaskStackHighWaterMark();
            }
        } else if (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL) {
            uart_flush_input(config_.port);
            xQueueReset(event_queue_);
            rx_errors_.fetch_add(1);
            decoder_.reset();
            ota_decoder_.reset();
        }
    }
}

void BridgeTransport::applyPendingOtaMode() {
    const uint32_t requested =
        ota_mode_request_sequence_.load(std::memory_order_acquire);
    if (requested ==
        ota_mode_applied_sequence_.load(std::memory_order_acquire))
        return;
    decoder_.reset();
    ota_decoder_.reset();
    // A mode switch is a frame boundary by definition; without this a mid-line
    // byte at switch time would leave the MKOT decoder unable to arm.
    ota_line_idle_ = true;
    ota_maintenance_.store(
        requested_ota_maintenance_.load(std::memory_order_acquire),
        std::memory_order_release);
    ota_mode_applied_sequence_.store(requested, std::memory_order_release);
}

void BridgeTransport::handleByte(uint8_t byte) {
    applyPendingOtaMode();
    if (otaRxArmed()) {
        // Claim the stream only from the exact MKOT magic at a line boundary,
        // exactly as Secondary::handleByte does on CH343.  Swallowing every
        // byte here made the right board's OTA_READY unparseable, and
        // handleOtaBridgeTextLine() is the ONLY consumer of
        // OTA_READY/OTA_PENDING/OTA_UNAVAILABLE -- so the text bootstrap that
        // retargets the link could never complete once maintenance was already
        // held for an older transaction.  That is the state
        // startRightMaintenanceBootstrap() is specifically written to run in.
        //
        // The binary bridge data plane stays closed: bridge frames go through
        // decoder_, which is not reachable from here.  Only MKOT and OTA
        // control text are audible while maintenance is held.
        if (ota_decoder_.buffered() != 0 ||
            (ota_line_idle_ && byte == makcm::ota::kMagic[0])) {
            const auto ota_result = ota_decoder_.feed(byte, ota_rx_frame_);
            if (ota_result == makcm::ota::FeedResult::FrameReady) {
                if (ota_frame_handler_ != nullptr)
                    ota_frame_handler_(context_, ota_rx_frame_);
                else
                    rx_errors_.fetch_add(1);
            } else if (ota_result == makcm::ota::FeedResult::FrameError) {
                rx_errors_.fetch_add(1);
            }
            return;
        }
        ota_line_idle_ = byte == '\n';
        text_bytes_.fetch_add(1);
        if (text_handler_ != nullptr) text_handler_(context_, byte);
        return;
    }
    makcm::bridge::Frame frame;
    uint8_t text_byte = 0;
    const auto result = decoder_.feed(byte, frame, text_byte);
    if (result == makcm::bridge::FeedResult::TextByte) {
        text_bytes_.fetch_add(1);
        if (text_handler_ != nullptr) text_handler_(context_, text_byte);
    } else if (result == makcm::bridge::FeedResult::FrameReady) {
        rx_frames_.fetch_add(1);
        if (frame.type == makcm::bridge::MessageType::EndpointInData ||
            frame.type == makcm::bridge::MessageType::EndpointIn) {
            input_sequence_.observe(frame.session, frame.sequence);
        }
        frame_handler_(context_, frame);
    } else if (result == makcm::bridge::FeedResult::FrameError) {
        rx_errors_.fetch_add(1);
    }
}

} // namespace makcm::native
