#pragma once

#include "BridgeProtocol.h"
#include <Arduino.h>
#include <USBHID.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

class DseHidDevice : public USBHIDDevice {
public:
    DseHidDevice();

    void begin();
    bool queueInputReport(const uint8_t *report, uint16_t length);
    void setFeatureSnapshot(const uint8_t *report, uint16_t length);

    void mouseMove(int x, int y, int wheel = 0, int pan = 0);
    void mousePress(uint8_t buttons);
    void mouseRelease(uint8_t buttons);

    uint16_t _onGetDescriptor(uint8_t *buffer) override;
    uint16_t _onGetFeature(uint8_t reportId, uint8_t *buffer, uint16_t length) override;
    void _onSetFeature(uint8_t reportId, const uint8_t *buffer, uint16_t length) override;
    void _onOutput(uint8_t reportId, const uint8_t *buffer, uint16_t length) override;

private:
    struct InputReport {
        uint8_t bytes[64];
        uint16_t length;
    };

    struct FeatureEntry {
        uint8_t reportId;
        uint8_t payloadLength;
        uint8_t payload[63];
    };

    struct MouseReport {
        uint8_t buttons;
        int8_t x;
        int8_t y;
        int8_t wheel;
        int8_t pan;
    } __attribute__((packed));

    static void inputTaskEntry(void *argument);
    void inputTask();
    FeatureEntry *findFeature(uint8_t reportId);
    void forwardHostReport(makcm::bridge::MessageType type,
                           uint8_t reportId,
                           const uint8_t *buffer,
                           uint16_t length);
    void sendMouseReport(int x, int y, int wheel, int pan);

    USBHID hid_;
    QueueHandle_t inputQueue_ = nullptr;
    TaskHandle_t inputTaskHandle_ = nullptr;
    SemaphoreHandle_t featureMutex_ = nullptr;
    uint8_t mouseButtons_ = 0;

    static constexpr uint8_t kMouseReportId = 0x7E;
    static constexpr size_t kFeatureCount = 20;
    FeatureEntry features_[kFeatureCount] = {};
};

extern DseHidDevice DseHid;
