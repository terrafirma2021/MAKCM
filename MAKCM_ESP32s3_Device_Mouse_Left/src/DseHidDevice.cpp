#include "DseHidDevice.h"

#include "BridgeProtocol.h"
#include "BridgeTransport.h"
#include <algorithm>
#include <cstring>

namespace {

// DualSense USB HID report descriptor. DualSense Edge uses the same 64-byte
// input layout and adds Fn/paddle state in the vendor-defined input bytes.
constexpr uint8_t kDseReportDescriptor[] = {
    0x05,0x01,0x09,0x05,0xA1,0x01,0x85,0x01,0x09,0x30,0x09,0x31,0x09,0x32,0x09,0x35,
    0x09,0x33,0x09,0x34,0x15,0x00,0x26,0xFF,0x00,0x75,0x08,0x95,0x06,0x81,0x02,0x06,
    0x00,0xFF,0x09,0x20,0x95,0x01,0x81,0x02,0x05,0x01,0x09,0x39,0x15,0x00,0x25,0x07,
    0x35,0x00,0x46,0x3B,0x01,0x65,0x14,0x75,0x04,0x95,0x01,0x81,0x42,0x65,0x00,0x05,
    0x09,0x19,0x01,0x29,0x0F,0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x0F,0x81,0x02,0x06,
    0x00,0xFF,0x09,0x21,0x95,0x0D,0x81,0x02,0x06,0x00,0xFF,0x09,0x22,0x15,0x00,0x26,
    0xFF,0x00,0x75,0x08,0x95,0x34,0x81,0x02,0x85,0x02,0x09,0x23,0x95,0x2F,0x91,0x02,
    0x85,0x05,0x09,0x33,0x95,0x28,0xB1,0x02,0x85,0x08,0x09,0x34,0x95,0x2F,0xB1,0x02,
    0x85,0x09,0x09,0x24,0x95,0x13,0xB1,0x02,0x85,0x0A,0x09,0x25,0x95,0x1A,0xB1,0x02,
    0x85,0x20,0x09,0x26,0x95,0x3F,0xB1,0x02,0x85,0x21,0x09,0x27,0x95,0x04,0xB1,0x02,
    0x85,0x22,0x09,0x40,0x95,0x3F,0xB1,0x02,0x85,0x80,0x09,0x28,0x95,0x3F,0xB1,0x02,
    0x85,0x81,0x09,0x29,0x95,0x3F,0xB1,0x02,0x85,0x82,0x09,0x2A,0x95,0x09,0xB1,0x02,
    0x85,0x83,0x09,0x2B,0x95,0x3F,0xB1,0x02,0x85,0x84,0x09,0x2C,0x95,0x3F,0xB1,0x02,
    0x85,0x85,0x09,0x2D,0x95,0x02,0xB1,0x02,0x85,0xA0,0x09,0x2E,0x95,0x01,0xB1,0x02,
    0x85,0xE0,0x09,0x2F,0x95,0x3F,0xB1,0x02,0x85,0xF0,0x09,0x30,0x95,0x3F,0xB1,0x02,
    0x85,0xF1,0x09,0x31,0x95,0x3F,0xB1,0x02,0x85,0xF2,0x09,0x32,0x95,0x0F,0xB1,0x02,
    0x85,0xF4,0x09,0x35,0x95,0x3F,0xB1,0x02,0x85,0xF5,0x09,0x36,0x95,0x03,0xB1,0x02,
    0xC0,

    // Keep the existing km.* API available as a second top-level collection.
    0x05,0x01,0x09,0x02,0xA1,0x01,0x85,0x7E,0x09,0x01,0xA1,0x00,0x05,0x09,0x19,0x01,
    0x29,0x05,0x15,0x00,0x25,0x01,0x95,0x05,0x75,0x01,0x81,0x02,0x95,0x01,0x75,0x03,
    0x81,0x01,0x05,0x01,0x09,0x30,0x09,0x31,0x09,0x38,0x15,0x81,0x25,0x7F,0x75,0x08,
    0x95,0x03,0x81,0x06,0x05,0x0C,0x0A,0x38,0x02,0x95,0x01,0x81,0x06,0xC0,0xC0,
};

constexpr struct {
    uint8_t id;
    uint8_t payloadLength;
} kFeatureLayout[] = {
    {0x05,40}, {0x08,47}, {0x09,19}, {0x0A,26}, {0x20,63},
    {0x21,4},  {0x22,63}, {0x80,63}, {0x81,63}, {0x82,9},
    {0x83,63}, {0x84,63}, {0x85,2},  {0xA0,1},  {0xE0,63},
    {0xF0,63}, {0xF1,63}, {0xF2,15}, {0xF4,63}, {0xF5,3},
};

static_assert(sizeof(kFeatureLayout) / sizeof(kFeatureLayout[0]) == 20,
              "Feature cache layout count must match DseHidDevice");

int8_t clampAxis(int value) {
    return static_cast<int8_t>(std::max(-127, std::min(127, value)));
}

} // namespace

DseHidDevice::DseHidDevice() {
    hid_.addDevice(this, sizeof(kDseReportDescriptor));
    for (size_t i = 0; i < kFeatureCount; ++i) {
        features_[i].reportId = kFeatureLayout[i].id;
        features_[i].payloadLength = kFeatureLayout[i].payloadLength;
        memset(features_[i].payload, 0, sizeof(features_[i].payload));
    }
}

void DseHidDevice::begin() {
    if (featureMutex_ == nullptr) {
        featureMutex_ = xSemaphoreCreateMutex();
    }
    if (inputQueue_ == nullptr) {
        inputQueue_ = xQueueCreate(1, sizeof(InputReport));
    }
    hid_.begin();
    if (inputTaskHandle_ == nullptr && inputQueue_ != nullptr) {
        xTaskCreate(inputTaskEntry, "DseInputTx", 3072, this, 4, &inputTaskHandle_);
    }
}

bool DseHidDevice::queueInputReport(const uint8_t *report, uint16_t length) {
    if (report == nullptr || length != 64 || report[0] != 0x01 || inputQueue_ == nullptr) {
        return false;
    }
    InputReport queued = {};
    queued.length = length;
    memcpy(queued.bytes, report, length);
    return xQueueOverwrite(inputQueue_, &queued) == pdTRUE;
}

void DseHidDevice::setFeatureSnapshot(const uint8_t *report, uint16_t length) {
    if (report == nullptr || length < 1 || length > 64 || featureMutex_ == nullptr) {
        return;
    }
    FeatureEntry *entry = findFeature(report[0]);
    if (entry == nullptr || xSemaphoreTake(featureMutex_, pdMS_TO_TICKS(2)) != pdTRUE) {
        return;
    }
    entry->payloadLength = static_cast<uint8_t>(length - 1);
    memcpy(entry->payload, report + 1, entry->payloadLength);
    xSemaphoreGive(featureMutex_);
}

void DseHidDevice::mouseMove(int x, int y, int wheel, int pan) {
    while (x != 0 || y != 0 || wheel != 0 || pan != 0) {
        const int8_t dx = clampAxis(x);
        const int8_t dy = clampAxis(y);
        const int8_t dw = clampAxis(wheel);
        const int8_t dp = clampAxis(pan);
        sendMouseReport(dx, dy, dw, dp);
        x -= dx;
        y -= dy;
        wheel -= dw;
        pan -= dp;
    }
}

void DseHidDevice::mousePress(uint8_t buttons) {
    mouseButtons_ |= buttons;
    sendMouseReport(0, 0, 0, 0);
}

void DseHidDevice::mouseRelease(uint8_t buttons) {
    mouseButtons_ &= static_cast<uint8_t>(~buttons);
    sendMouseReport(0, 0, 0, 0);
}

uint16_t DseHidDevice::_onGetDescriptor(uint8_t *buffer) {
    memcpy(buffer, kDseReportDescriptor, sizeof(kDseReportDescriptor));
    return sizeof(kDseReportDescriptor);
}

uint16_t DseHidDevice::_onGetFeature(uint8_t reportId, uint8_t *buffer, uint16_t length) {
    FeatureEntry *entry = findFeature(reportId);
    if (entry == nullptr || buffer == nullptr || featureMutex_ == nullptr ||
        xSemaphoreTake(featureMutex_, pdMS_TO_TICKS(2)) != pdTRUE) {
        return 0;
    }
    const uint16_t copyLength = std::min<uint16_t>(length, entry->payloadLength);
    memcpy(buffer, entry->payload, copyLength);
    xSemaphoreGive(featureMutex_);
    return copyLength;
}

void DseHidDevice::_onSetFeature(uint8_t reportId, const uint8_t *buffer, uint16_t length) {
    // Arduino-ESP32 2.0.16 routes a control-endpoint SET_REPORT with a
    // non-zero report ID through _onSetFeature even when the host requested
    // an OUTPUT report. Report 0x02 is output-only in the DSE descriptor, so
    // keep that path byte-for-byte equivalent to interrupt OUT.
    const makcm::bridge::MessageType type =
        reportId == 0x02 ? makcm::bridge::MessageType::DseOutput
                         : makcm::bridge::MessageType::DseFeatureSet;
    forwardHostReport(type,
                      reportId, buffer, length);
}

void DseHidDevice::_onOutput(uint8_t reportId, const uint8_t *buffer, uint16_t length) {
    forwardHostReport(makcm::bridge::MessageType::DseOutput,
                      reportId, buffer, length);
}

void DseHidDevice::inputTaskEntry(void *argument) {
    static_cast<DseHidDevice *>(argument)->inputTask();
}

void DseHidDevice::inputTask() {
    InputReport report = {};
    while (true) {
        if (xQueueReceive(inputQueue_, &report, portMAX_DELAY) == pdTRUE &&
            report.length == 64 && report.bytes[0] == 0x01) {
            hid_.SendReport(report.bytes[0], report.bytes + 1, report.length - 1, 10);
        }
    }
}

DseHidDevice::FeatureEntry *DseHidDevice::findFeature(uint8_t reportId) {
    for (auto &entry : features_) {
        if (entry.reportId == reportId) {
            return &entry;
        }
    }
    return nullptr;
}

void DseHidDevice::forwardHostReport(makcm::bridge::MessageType type,
                                     uint8_t reportId,
                                     const uint8_t *buffer,
                                     uint16_t length) {
    if (length > 63 || (length != 0 && buffer == nullptr)) {
        return;
    }
    uint8_t raw[64] = {};
    raw[0] = reportId;
    if (length != 0) {
        memcpy(raw + 1, buffer, length);
    }
    sendBridgeFrame(type, raw, length + 1);
}

void DseHidDevice::sendMouseReport(int x, int y, int wheel, int pan) {
    MouseReport report = {
        mouseButtons_, clampAxis(x), clampAxis(y), clampAxis(wheel), clampAxis(pan)
    };
    hid_.SendReport(kMouseReportId, &report, sizeof(report), 10);
}
