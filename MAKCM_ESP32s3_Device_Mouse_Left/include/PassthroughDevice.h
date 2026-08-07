#pragma once

#include "BridgeProtocol.h"
#include "EndpointTxPhase.h"
#include "HidMouseReport.h"
#include "MouseInjectionLedger.h"
#include "MouseInjectionRateGate.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include "tusb.h"

class PassthroughDevice {
public:
    PassthroughDevice() = default;

    void resetProfile();
    bool beginProfile(const makcm::bridge::DeviceProfile &profile);
    bool appendDeviceDescriptor(uint16_t offset, const uint8_t *data,
                                uint16_t length);
    bool appendConfigurationDescriptor(uint16_t offset, const uint8_t *data,
                                       uint16_t length);
    // Kept for the v3 call sites while the profile transport migrates. Raw
    // HID report descriptors are fetched through the generic EP0 proxy.
    bool appendReportDescriptor(uint16_t offset, const uint8_t *data,
                                uint16_t length);
    bool appendHidReportDescriptor(uint8_t interfaceNumber, uint16_t offset,
                                   const uint8_t *data, uint16_t length);
    bool appendStringDescriptor(uint8_t index, uint16_t langid,
                                uint16_t offset, const uint8_t *data,
                                uint16_t length);
    bool finishProfile();
    bool beginUsb();
    void disconnectUsb();
    void reconnectUsb();

    bool queueEndpointInput(uint8_t endpoint, const uint8_t *data,
                            uint16_t length);
    bool queueInput(makcm::bridge::MessageType type,
                    const uint8_t *data, uint16_t length);
    void acceptControlResponse(const uint8_t *data, uint16_t length);
    void acceptFeatureResponse(const uint8_t *data, uint16_t length);

    bool mouseMove(int64_t x, int64_t y, int64_t wheel = 0,
                   int64_t pan = 0);
    void mouseButton(uint8_t button, bool pressed);
    void configureMouseInjection(bool enabled, uint16_t rateHz = 0);
    void printStatus(Print &out);
    bool inputEndpointReady();
    bool isMouseInputEndpoint(uint8_t endpoint) const {
        return endpoint != 0 && endpoint == mouseInEndpoint_;
    }
    bool isXinputEndpoint(uint8_t endpoint) const {
        return endpoint != 0 && endpoint == xinputInEndpoint_;
    }
    uint8_t mouseButtons(const uint8_t *report, uint16_t length) const;

    bool profileReady() const { return profileReady_; }
    const makcm::bridge::DeviceProfile &profile() const { return profile_; }
    uint16_t descriptorBytesReceived() const {
        return deviceDescriptorBytes_ + configurationDescriptorBytes_;
    }

    const uint8_t *deviceDescriptor() const { return deviceDescriptor_; }
    const uint8_t *configurationDescriptor() const {
        return configurationDescriptor_;
    }
    uint16_t configurationDescriptorLength() const {
        return configurationDescriptorBytes_;
    }
    const uint16_t *stringDescriptor(uint8_t index, uint16_t langid) const;

    // TinyUSB application class-driver entry points.
    void driverInit();
    void driverReset(uint8_t rhport);
    uint16_t driverOpen(uint8_t rhport, const uint8_t *interfaceDescriptor,
                        uint16_t maxLength);
    bool driverControl(uint8_t rhport, uint8_t stage,
                       const tusb_control_request_t *request);
    bool driverTransfer(uint8_t rhport, uint8_t endpoint, uint8_t result,
                        uint32_t transferred);

private:
    static constexpr uint8_t kEndpointSlots = 6;
    static constexpr uint16_t kMaxPacketSize = 64;
    static constexpr uint8_t kEndpointQueueDepth = 16;

    using EndpointPhase = makcm::usb::EndpointPhase;
    using EndpointCompletion = makcm::usb::EndpointCompletion;

    struct EndpointState {
        bool inPresent = false;
        bool outPresent = false;
        uint16_t inPacketSize = 0;
        uint16_t outPacketSize = 0;
        alignas(4) uint8_t outBuffer[kMaxPacketSize] = {};
        alignas(4) uint8_t activeIn[kMaxPacketSize] = {};
        alignas(4) uint8_t activeOriginal[kMaxPacketSize] = {};
        uint16_t activeOriginalLength = 0;
        makcm::usb::EndpointTxPhase inTransfer;
        makcm::usb::EndpointTxPhase outTransfer;
        alignas(4) uint8_t retryIn[kMaxPacketSize] = {};
        uint16_t retryInLength = 0;
        bool retryInValid = false;
        bool forceSyntheticNext = false;
        bool outNeedsRearm = false;
        bool activeInPhysical = false;
        bool activeInSynthetic = false;
        bool activeInMouseMerged = false;
        bool activeNeedsStandalone = false;
        uint32_t queued = 0;
        uint32_t queueDrops = 0;
        // Controller reports are absolute state snapshots. When the upstream
        // host is slower than the physical 1 kHz producer, retaining an old
        // FIFO tail adds input latency and eventually discards the newest
        // state. Keep only the newest queued XInput snapshot instead.
        uint32_t queueCoalesced = 0;
        uint32_t transfersStarted = 0;
        uint32_t transfersCompleted = 0;
        uint8_t queueHighWater = 0;
    };

    struct InputItem {
        uint32_t epoch = 0;
        uint16_t length = 0;
        uint8_t data[kMaxPacketSize] = {};
    };

    struct OutputItem {
        uint32_t epoch = 0;
        uint32_t bridgeSession = 0;
        uint8_t endpoint = 0;
        uint16_t length = 0;
        uint8_t data[kMaxPacketSize] = {};
    };

    struct HidDescriptorState {
        uint8_t interfaceNumber = 0xFF;
        uint16_t expectedLength = 0;
        uint16_t receivedLength = 0;
        uint8_t data[makcm::bridge::kMaxReportDescriptorSize] = {};
    };

    struct StringDescriptorState {
        uint8_t index = 0;
        uint16_t langid = 0;
        uint16_t receivedLength = 0;
        alignas(2) uint8_t data[makcm::bridge::kMaxStringDescriptorSize] = {};
    };

    static void inputTaskEntry(void *argument);
    void inputTask();
    static void outputTaskEntry(void *argument);
    void outputTask();
    void notifyInputTask();
    bool schedulerWorkPending() const;
    bool pumpEndpoint(uint8_t number);
    bool pumpOutputEndpoint(uint8_t number);
    bool prepareMousePhysical(const InputItem &item, uint8_t *output,
                              uint16_t &length,
                              makcm::hid::MouseMergeDelta &consumed,
                              uint32_t &buttonGeneration);
    bool prepareMouseSynthetic(uint8_t *output, uint16_t &length,
                               makcm::hid::MouseMergeDelta &consumed,
                               uint32_t &buttonGeneration);
    void settleMouseInjection(bool success);
    bool mouseInjectionPending() const;
    bool mouseMovementPending() const;
    bool mouseInjectionAllowed(uint64_t nowUs) const;
    void noteMouseInjectionSubmit(uint64_t nowUs);
    bool validateConfiguration();
    bool validateStringDescriptors() const;
    void discoverMouseEndpoint();
    const HidDescriptorState *findHidDescriptor(uint8_t interfaceNumber) const;
    bool forwardControl(const tusb_control_request_t &request,
                        const uint8_t *outData, uint16_t outLength,
                        uint8_t *inData, uint16_t &inLength);
    bool forwardControlOneWay(const tusb_control_request_t &request,
                              const uint8_t *outData, uint16_t outLength);
    bool addMouseDelta(int64_t x, int64_t y, int64_t wheel, int64_t pan);

    makcm::bridge::DeviceProfile profile_ = {};
    uint8_t deviceDescriptor_[18] = {};
    uint8_t configurationDescriptor_[
        makcm::bridge::kMaxConfigurationDescriptorSize] = {};
    uint16_t deviceDescriptorBytes_ = 0;
    uint16_t configurationDescriptorBytes_ = 0;
    bool profileReady_ = false;
    bool usbStarted_ = false;
    bool topologySupported_ = false;

    EndpointState endpoints_[kEndpointSlots] = {};
    mutable portMUX_TYPE schedulerMux_ = portMUX_INITIALIZER_UNLOCKED;
    uint32_t schedulerEpoch_ = 1;
    HidDescriptorState hidDescriptors_[makcm::bridge::kMaxHidInterfaces] = {};
    uint8_t hidDescriptorCount_ = 0;
    StringDescriptorState
        stringDescriptors_[makcm::bridge::kMaxStringDescriptors] = {};
    uint8_t stringDescriptorCount_ = 0;
    QueueHandle_t inputQueues_[kEndpointSlots] = {};
    TaskHandle_t inputTaskHandle_ = nullptr;
    QueueHandle_t outputQueue_ = nullptr;
    TaskHandle_t outputTaskHandle_ = nullptr;

    SemaphoreHandle_t controlMutex_ = nullptr;
    SemaphoreHandle_t controlDone_ = nullptr;
    volatile uint16_t pendingControlId_ = 0;
    uint16_t nextControlId_ = 1;
    makcm::bridge::ControlStatus controlStatus_ =
        makcm::bridge::ControlStatus::Error;
    uint8_t controlResponse_[makcm::bridge::kMaxPayloadSize] = {};
    uint16_t controlResponseLength_ = 0;
    tusb_control_request_t pendingOutRequest_ = {};
    bool pendingOutRequestValid_ = false;
    uint8_t controlOutBuffer_[makcm::bridge::kMaxPayloadSize] = {};

    uint8_t mouseInEndpoint_ = 0;
    uint8_t xinputInEndpoint_ = 0;
    mutable portMUX_TYPE mouseMux_ = portMUX_INITIALIZER_UNLOCKED;
    makcm::hid::MouseInjectionLedger mouseLedger_;
    makcm::hid::MouseInjectionRateGate mouseRateGate_;
    alignas(4) uint8_t lastMouseInput_[kMaxPacketSize] = {};
    uint16_t lastMouseInputLength_ = 0;
    makcm::hid::MouseReportLayout mouseLayout_ = {};

    volatile uint32_t inputReceived_ = 0;
    volatile uint32_t inputQueued_ = 0;
    volatile uint32_t inputQueueDrops_ = 0;
    volatile uint32_t endpointSendAttempts_ = 0;
    volatile uint32_t endpointSendSuccess_ = 0;
    volatile uint32_t endpointSendErrors_ = 0;
    volatile uint32_t endpointShortTransfers_ = 0;
    volatile uint32_t endpointStaleCompletions_ = 0;
    volatile uint32_t endpointStaleInputs_ = 0;
    volatile uint32_t endpointRetryPolls_ = 0;
    volatile uint32_t endpointOutputDrops_ = 0;
    volatile uint32_t syntheticRequests_ = 0;
    volatile uint32_t syntheticComposed_ = 0;
    volatile uint32_t syntheticQueued_ = 0;
    volatile uint32_t syntheticQueueErrors_ = 0;
    volatile uint32_t syntheticMerged_ = 0;
    volatile uint32_t syntheticTransfersStarted_ = 0;
    volatile uint32_t syntheticTransfersCompleted_ = 0;
    volatile uint32_t syntheticSuppressed_ = 0;
    volatile uint32_t controlRequests_ = 0;
    volatile uint32_t controlOneWay_ = 0;
    volatile uint32_t controlErrors_ = 0;
};

extern PassthroughDevice Passthrough;
