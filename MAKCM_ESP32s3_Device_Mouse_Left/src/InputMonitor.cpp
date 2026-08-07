#include "InputMonitor.h"
#include "MakcuWireTrace.h"

#include <atomic>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace {

#define MAKCM_STRINGIFY_IMPL(value) #value
#define MAKCM_STRINGIFY(value) MAKCM_STRINGIFY_IMPL(value)

constexpr uint8_t kFireBit = 1u << 0; // R2 -> physical LMB/fire
constexpr uint8_t kAdsBit = 1u << 1;  // L2 -> physical RMB/ADS
constexpr uint8_t kTriggerPress = 24;
constexpr uint8_t kTriggerRelease = 12;
constexpr uint32_t kDseStaleMs = 100;
constexpr uint32_t kFastBaud = 4000000;

constexpr uint8_t kSpeedSwitch[] = {
    0xDE, 0xAD, 0x05, 0x00, 0xA5, 0x00, 0x09, 0x3D, 0x00
};

std::atomic<uint8_t> dseMask{0};
std::atomic<uint8_t> mouseMask{0};
std::atomic<uint32_t> dseLastReportMs{0};
std::atomic<bool> streamEnabled{false};
std::atomic<uint16_t> streamPeriodMs{0};
std::atomic<uint32_t> stateGeneration{0};
std::atomic<bool> fastBaudPending{false};
SemaphoreHandle_t serial0TxMutex = nullptr;
TaskHandle_t monitorTaskHandle = nullptr;
size_t speedSwitchIndex = 0;

void setMaskBit(std::atomic<uint8_t> &mask, uint8_t bit, bool down) {
    uint8_t oldValue = mask.load(std::memory_order_relaxed);
    while (true) {
        const uint8_t newValue = down ? static_cast<uint8_t>(oldValue | bit)
                                      : static_cast<uint8_t>(oldValue & ~bit);
        if (newValue == oldValue) {
            return;
        }
        if (mask.compare_exchange_weak(oldValue, newValue,
                                      std::memory_order_release,
                                      std::memory_order_relaxed)) {
            stateGeneration.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
}

uint8_t currentMask() {
    return static_cast<uint8_t>(dseMask.load(std::memory_order_acquire) |
                                mouseMask.load(std::memory_order_acquire));
}

void writeButtonFrame(uint8_t mask) {
    if (serial0TxMutex == nullptr ||
        xSemaphoreTake(serial0TxMutex, pdMS_TO_TICKS(5)) != pdTRUE) {
        return;
    }
    static const uint8_t prefix[] = {'k', 'm', '.'};
    static const uint8_t suffix[] = {'\r', '\n', '>', '>', '>', ' '};
    const uint8_t frame[] = {
        prefix[0], prefix[1], prefix[2], mask,
        suffix[0], suffix[1], suffix[2], suffix[3], suffix[4], suffix[5]
    };
    MakcuWireTrace::record(MakcuWireTrace::Direction::Tx,
                           frame, sizeof(frame));
    Serial0.write(prefix, sizeof(prefix));
    Serial0.write(mask);
    Serial0.write(suffix, sizeof(suffix));
    xSemaphoreGive(serial0TxMutex);
}

void writeTextReply(const char *text) {
    if (text == nullptr || serial0TxMutex == nullptr ||
        xSemaphoreTake(serial0TxMutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;
    }
    char frame[192];
    const int length = snprintf(frame, sizeof(frame), "%s\r\n>>> ", text);
    if (length > 0) {
        const size_t captured = static_cast<size_t>(length) < sizeof(frame)
            ? static_cast<size_t>(length) : sizeof(frame) - 1;
        MakcuWireTrace::record(MakcuWireTrace::Direction::Tx,
                               reinterpret_cast<const uint8_t *>(frame),
                               captured);
    }
    Serial0.print(text);
    Serial0.print("\r\n>>> ");
    xSemaphoreGive(serial0TxMutex);
}

void monitorTask(void *) {
    uint32_t seenGeneration = stateGeneration.load(std::memory_order_relaxed);
    uint32_t lastPublishMs = 0;

    while (true) {
        if (fastBaudPending.exchange(false, std::memory_order_acq_rel)) {
            Serial0.flush();
            Serial0.updateBaudRate(kFastBaud);
        }

        const uint32_t now = millis();
        const uint32_t lastDse = dseLastReportMs.load(std::memory_order_acquire);
        if (lastDse != 0 && now - lastDse > kDseStaleMs &&
            dseMask.exchange(0, std::memory_order_acq_rel) != 0) {
            stateGeneration.fetch_add(1, std::memory_order_relaxed);
            dseLastReportMs.store(0, std::memory_order_release);
        }

        if (streamEnabled.load(std::memory_order_acquire)) {
            const uint32_t generation = stateGeneration.load(std::memory_order_acquire);
            const uint16_t period = streamPeriodMs.load(std::memory_order_relaxed);
            const bool changed = generation != seenGeneration;
            const bool periodic = period != 0 && now - lastPublishMs >= period;
            if (changed || periodic || lastPublishMs == 0) {
                writeButtonFrame(currentMask());
                seenGeneration = generation;
                lastPublishMs = now;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

bool parseDown(const char *command, const char *name, bool &down) {
    if (command == nullptr || name == nullptr) {
        return false;
    }
    char expected[24];
    snprintf(expected, sizeof(expected), "km.%s(1)", name);
    if (strcmp(command, expected) == 0) {
        down = true;
        return true;
    }
    snprintf(expected, sizeof(expected), "km.%s(0)", name);
    if (strcmp(command, expected) == 0) {
        down = false;
        return true;
    }
    return false;
}

} // namespace

void beginInputMonitor() {
    if (serial0TxMutex == nullptr) {
        serial0TxMutex = xSemaphoreCreateMutex();
    }
    if (monitorTaskHandle == nullptr) {
        xTaskCreate(monitorTask, "InputMonitor", 2304, nullptr, 3,
                    &monitorTaskHandle);
    }
}

void resetInputMonitor() {
    const uint8_t oldController = dseMask.exchange(0, std::memory_order_acq_rel);
    const uint8_t oldMouse = mouseMask.exchange(0, std::memory_order_acq_rel);
    dseLastReportMs.store(0, std::memory_order_release);
    if (oldController != 0 || oldMouse != 0) {
        stateGeneration.fetch_add(1, std::memory_order_relaxed);
    }
}

void updateDseTriggerMonitor(const uint8_t *report, uint16_t length) {
    if (report == nullptr || length != 64 || report[0] != 0x01) {
        return;
    }

    const uint8_t oldMask = dseMask.load(std::memory_order_relaxed);
    const bool oldFire = (oldMask & kFireBit) != 0;
    const bool oldAds = (oldMask & kAdsBit) != 0;
    const bool fire = oldFire ? report[6] > kTriggerRelease
                              : report[6] >= kTriggerPress;
    const bool ads = oldAds ? report[5] > kTriggerRelease
                            : report[5] >= kTriggerPress;
    const uint8_t newMask = static_cast<uint8_t>((fire ? kFireBit : 0) |
                                                  (ads ? kAdsBit : 0));
    dseLastReportMs.store(millis(), std::memory_order_release);
    if (dseMask.exchange(newMask, std::memory_order_acq_rel) != newMask) {
        stateGeneration.fetch_add(1, std::memory_order_relaxed);
    }
}

void updateXboxTriggerMonitor(const uint8_t *report, uint16_t length) {
    // Standard Xbox 360 USB input packet: 00 14, buttons[2..3], LT[4], RT[5].
    // This observer never rewrites the packet forwarded to the upstream host.
    if (report == nullptr || length < 6 || report[0] != 0x00 ||
        report[1] < 0x06) {
        return;
    }
    const uint8_t oldMask = dseMask.load(std::memory_order_relaxed);
    const bool oldFire = (oldMask & kFireBit) != 0;
    const bool oldAds = (oldMask & kAdsBit) != 0;
    const bool fire = oldFire ? report[5] > kTriggerRelease
                              : report[5] >= kTriggerPress;
    const bool ads = oldAds ? report[4] > kTriggerRelease
                            : report[4] >= kTriggerPress;
    const uint8_t newMask = static_cast<uint8_t>((fire ? kFireBit : 0) |
                                                  (ads ? kAdsBit : 0));
    dseLastReportMs.store(millis(), std::memory_order_release);
    if (dseMask.exchange(newMask, std::memory_order_acq_rel) != newMask) {
        stateGeneration.fetch_add(1, std::memory_order_relaxed);
    }
}

void updateMouseButtonMonitor(const uint8_t *report, uint16_t length,
                              bool reportIdPrefix) {
    const uint16_t buttonOffset = reportIdPrefix ? 1 : 0;
    if (report == nullptr || length <= buttonOffset) {
        return;
    }

    const uint8_t newMask = static_cast<uint8_t>(report[buttonOffset] & 0x1F);
    if (mouseMask.exchange(newMask, std::memory_order_acq_rel) != newMask) {
        stateGeneration.fetch_add(1, std::memory_order_relaxed);
    }
}

void observeRightMouseCommand(const char *command) {
    bool down = false;
    if (parseDown(command, "left", down)) {
        setMaskBit(mouseMask, 1u << 0, down);
    } else if (parseDown(command, "right", down)) {
        setMaskBit(mouseMask, 1u << 1, down);
    } else if (parseDown(command, "middle", down)) {
        setMaskBit(mouseMask, 1u << 2, down);
    } else if (parseDown(command, "side1", down)) {
        setMaskBit(mouseMask, 1u << 3, down);
    } else if (parseDown(command, "side2", down)) {
        setMaskBit(mouseMask, 1u << 4, down);
    }
}

void handleKmVersionCommand(const char *) {
    writeTextReply("MAKCM DSE bridge " MAKCM_STRINGIFY(FIRMWARE_VERSION));
}

void handleKmEchoCommand(const char *) {
    // The bridge always keeps command echo disabled. A short prompt still
    // gives MakcuDevice an independent liveness reply.
    writeTextReply("OK");
}

void handleKmButtonsCommand(const char *command) {
    int enabled = 0;
    int period = -1;
    if (command == nullptr ||
        sscanf(command, "km.buttons(%d,%d)", &enabled, &period) < 1) {
        writeTextReply("ERR");
        return;
    }
    if (period >= 0) {
        streamPeriodMs.store(static_cast<uint16_t>(constrain(period, 1, 1000)),
                             std::memory_order_release);
    }
    streamEnabled.store(enabled != 0, std::memory_order_release);
    stateGeneration.fetch_add(1, std::memory_order_relaxed);
    if (enabled != 0) {
        writeButtonFrame(currentMask());
    }
}

bool consumeMakcuSpeedSwitchByte(uint8_t byte) {
    if (byte == kSpeedSwitch[speedSwitchIndex]) {
        ++speedSwitchIndex;
        if (speedSwitchIndex == sizeof(kSpeedSwitch)) {
            speedSwitchIndex = 0;
            fastBaudPending.store(true, std::memory_order_release);
        }
        return true;
    }
    speedSwitchIndex = byte == kSpeedSwitch[0] ? 1 : 0;
    return speedSwitchIndex != 0;
}
