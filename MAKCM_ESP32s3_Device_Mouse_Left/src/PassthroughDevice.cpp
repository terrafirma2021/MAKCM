#include "PassthroughDevice.h"

#include "BridgeTransport.h"
#include "device/usbd_pvt.h"
#include <USB.h>
#include <algorithm>
#include <cstring>
#include <esp_timer.h>

namespace {

PassthroughDevice *gPassthrough = nullptr;

bool normalizeInjectableMouseLayout(
    makcm::hid::MouseReportLayout &layout) {
    if (!layout.valid || !layout.x.present || !layout.y.present ||
        layout.x.logicalMinimum >= 0 || layout.x.logicalMaximum <= 0 ||
        layout.y.logicalMinimum >= 0 || layout.y.logicalMaximum <= 0) {
        return false;
    }
    if (layout.wheel.present &&
        (layout.wheel.logicalMinimum >= 0 ||
         layout.wheel.logicalMaximum <= 0)) {
        layout.wheel = {};
    }
    if (layout.pan.present &&
        (layout.pan.logicalMinimum >= 0 || layout.pan.logicalMaximum <= 0)) {
        layout.pan = {};
    }
    return true;
}

void proxyInit() {
    if (gPassthrough != nullptr) {
        gPassthrough->driverInit();
    }
}

void proxyReset(uint8_t rhport) {
    if (gPassthrough != nullptr) {
        gPassthrough->driverReset(rhport);
    }
}

uint16_t proxyOpen(uint8_t rhport, tusb_desc_interface_t const *descriptor,
                   uint16_t maxLength) {
    return gPassthrough == nullptr
        ? 0
        : gPassthrough->driverOpen(
              rhport, reinterpret_cast<const uint8_t *>(descriptor), maxLength);
}

bool proxyControl(uint8_t rhport, uint8_t stage,
                  tusb_control_request_t const *request) {
    return gPassthrough != nullptr &&
           gPassthrough->driverControl(rhport, stage, request);
}

bool proxyTransfer(uint8_t rhport, uint8_t endpoint, xfer_result_t result,
                   uint32_t transferred) {
    return gPassthrough != nullptr &&
           gPassthrough->driverTransfer(
               rhport, endpoint, static_cast<uint8_t>(result), transferred);
}

const usbd_class_driver_t kProxyDriver = {
#if CFG_TUSB_DEBUG >= CFG_TUD_LOG_LEVEL
    "MAKCM-PROXY",
#endif
    proxyInit,
    proxyReset,
    proxyOpen,
    proxyControl,
    proxyTransfer,
    nullptr,
};

} // namespace

PassthroughDevice Passthrough;

extern "C" usbd_class_driver_t const *usbd_app_driver_get_cb(
    uint8_t *driverCount) {
    *driverCount = 1;
    return &kProxyDriver;
}

// The Arduino core owns tud_vendor_control_xfer_cb(), but deliberately exposes
// this weak hook. Route every vendor request through the same raw EP0 proxy as
// interface-class requests.
extern "C" bool tinyusb_vendor_control_request_cb(
    uint8_t rhport, uint8_t stage, tusb_control_request_t const *request) {
    return gPassthrough != nullptr &&
           gPassthrough->driverControl(rhport, stage, request);
}

void PassthroughDevice::resetProfile() {
    // End the current physical-device session before touching descriptors.
    // Any dequeued item or late DCD completion carries the previous epoch and
    // is ignored instead of leaking into the next attached device.
    portENTER_CRITICAL(&schedulerMux_);
    if (++schedulerEpoch_ == 0) ++schedulerEpoch_;
    memset(endpoints_, 0, sizeof(endpoints_));
    portENTER_CRITICAL(&mouseMux_);
    mouseLedger_.reset();
    mouseRateGate_.resetTiming();
    lastMouseInputLength_ = 0;
    memset(lastMouseInput_, 0, sizeof(lastMouseInput_));
    portEXIT_CRITICAL(&mouseMux_);
    portEXIT_CRITICAL(&schedulerMux_);

    profile_ = {};
    memset(deviceDescriptor_, 0, sizeof(deviceDescriptor_));
    memset(configurationDescriptor_, 0, sizeof(configurationDescriptor_));
    memset(hidDescriptors_, 0, sizeof(hidDescriptors_));
    memset(stringDescriptors_, 0, sizeof(stringDescriptors_));
    deviceDescriptorBytes_ = 0;
    configurationDescriptorBytes_ = 0;
    hidDescriptorCount_ = 0;
    stringDescriptorCount_ = 0;
    profileReady_ = false;
    topologySupported_ = false;
    mouseInEndpoint_ = 0;
    xinputInEndpoint_ = 0;
    mouseLayout_ = {};
    for (uint8_t i = 1; i < kEndpointSlots; ++i) {
        if (inputQueues_[i] != nullptr) xQueueReset(inputQueues_[i]);
    }
    if (outputQueue_ != nullptr) xQueueReset(outputQueue_);
}

bool PassthroughDevice::beginProfile(
    const makcm::bridge::DeviceProfile &profile) {
    if (profile.version != makcm::bridge::kProtocolVersion ||
        profile.kind != makcm::bridge::DeviceKind::RawUsb ||
        profile.deviceDescriptorLength != sizeof(deviceDescriptor_) ||
        profile.configurationDescriptorLength < 9 ||
        profile.configurationDescriptorLength >
            sizeof(configurationDescriptor_)) {
        return false;
    }
    resetProfile();
    profile_ = profile;
    return true;
}

bool PassthroughDevice::appendDeviceDescriptor(uint16_t offset,
                                               const uint8_t *data,
                                               uint16_t length) {
    if (data == nullptr || offset != deviceDescriptorBytes_ ||
        static_cast<uint32_t>(offset) + length > sizeof(deviceDescriptor_) ||
        static_cast<uint32_t>(offset) + length >
            profile_.deviceDescriptorLength) {
        return false;
    }
    memcpy(deviceDescriptor_ + offset, data, length);
    deviceDescriptorBytes_ += length;
    return true;
}

bool PassthroughDevice::appendConfigurationDescriptor(uint16_t offset,
                                                      const uint8_t *data,
                                                      uint16_t length) {
    if (data == nullptr || offset != configurationDescriptorBytes_ ||
        static_cast<uint32_t>(offset) + length >
            sizeof(configurationDescriptor_) ||
        static_cast<uint32_t>(offset) + length >
            profile_.configurationDescriptorLength) {
        return false;
    }
    memcpy(configurationDescriptor_ + offset, data, length);
    configurationDescriptorBytes_ += length;
    return true;
}

bool PassthroughDevice::appendReportDescriptor(uint16_t offset,
                                               const uint8_t *data,
                                               uint16_t length) {
    const uint8_t interfaceNumber = profile_.interfaceNumber;
    return appendHidReportDescriptor(interfaceNumber, offset, data, length);
}

bool PassthroughDevice::appendHidReportDescriptor(uint8_t interfaceNumber,
                                                  uint16_t offset,
                                                  const uint8_t *data,
                                                  uint16_t length) {
    if (data == nullptr || interfaceNumber == 0xFF ||
        static_cast<uint32_t>(offset) + length >
            makcm::bridge::kMaxReportDescriptorSize) {
        return false;
    }
    HidDescriptorState *slot = nullptr;
    for (uint8_t i = 0; i < hidDescriptorCount_; ++i) {
        if (hidDescriptors_[i].interfaceNumber == interfaceNumber) {
            slot = &hidDescriptors_[i];
            break;
        }
    }
    if (slot == nullptr) {
        if (hidDescriptorCount_ >= makcm::bridge::kMaxHidInterfaces ||
            offset != 0) {
            return false;
        }
        slot = &hidDescriptors_[hidDescriptorCount_++];
        slot->interfaceNumber = interfaceNumber;
    }
    if (offset != slot->receivedLength) {
        return false;
    }
    memcpy(slot->data + offset, data, length);
    slot->receivedLength += length;
    return true;
}

bool PassthroughDevice::appendStringDescriptor(
    uint8_t index, uint16_t langid, uint16_t offset,
    const uint8_t *data, uint16_t length) {
    if (data == nullptr || length == 0 ||
        static_cast<uint32_t>(offset) + length >
            makcm::bridge::kMaxStringDescriptorSize) {
        return false;
    }
    StringDescriptorState *slot = nullptr;
    for (uint8_t i = 0; i < stringDescriptorCount_; ++i) {
        if (stringDescriptors_[i].index == index &&
            stringDescriptors_[i].langid == langid) {
            slot = &stringDescriptors_[i];
            break;
        }
    }
    if (slot == nullptr) {
        if (stringDescriptorCount_ >= makcm::bridge::kMaxStringDescriptors ||
            offset != 0) {
            return false;
        }
        slot = &stringDescriptors_[stringDescriptorCount_++];
        slot->index = index;
        slot->langid = langid;
    }
    if (slot->receivedLength != offset) return false;
    memcpy(slot->data + offset, data, length);
    slot->receivedLength += length;
    return slot->receivedLength < 2 ||
        (slot->data[1] == TUSB_DESC_STRING && slot->data[0] >= 2 &&
         slot->data[0] >= slot->receivedLength);
}

bool PassthroughDevice::validateStringDescriptors() const {
    auto hasIndex = [this](uint8_t index) {
        if (index == 0) return true;
        for (uint8_t i = 0; i < stringDescriptorCount_; ++i) {
            const StringDescriptorState &slot = stringDescriptors_[i];
            if (slot.index == index && slot.receivedLength >= 2 &&
                slot.data[1] == TUSB_DESC_STRING &&
                slot.data[0] == slot.receivedLength) {
                return true;
            }
        }
        return false;
    };
    if (stringDescriptorCount_ == 0 ||
        stringDescriptors_[0].index != 0 ||
        stringDescriptors_[0].receivedLength < 4) {
        return false;
    }
    if (!hasIndex(deviceDescriptor_[14]) ||
        !hasIndex(deviceDescriptor_[15]) ||
        !hasIndex(deviceDescriptor_[16]) ||
        !hasIndex(configurationDescriptor_[6])) {
        return false;
    }
    for (uint16_t offset = 0; offset < configurationDescriptorBytes_;) {
        const uint8_t length = configurationDescriptor_[offset];
        if (length < 2 || offset + length > configurationDescriptorBytes_) {
            return false;
        }
        const uint8_t type = configurationDescriptor_[offset + 1];
        if (type == TUSB_DESC_INTERFACE && length >= 9 &&
            !hasIndex(configurationDescriptor_[offset + 8])) {
            return false;
        }
        if (type == TUSB_DESC_INTERFACE_ASSOCIATION && length >= 8 &&
            !hasIndex(configurationDescriptor_[offset + 7])) {
            return false;
        }
        offset += length;
    }
    return true;
}

bool PassthroughDevice::validateConfiguration() {
    const uint16_t bcdUsb = static_cast<uint16_t>(deviceDescriptor_[2]) |
        (static_cast<uint16_t>(deviceDescriptor_[3]) << 8);
    if (deviceDescriptorBytes_ != sizeof(deviceDescriptor_) ||
        configurationDescriptorBytes_ !=
            profile_.configurationDescriptorLength ||
        deviceDescriptor_[0] != 18 || deviceDescriptor_[1] != TUSB_DESC_DEVICE ||
        bcdUsb > 0x0200 || deviceDescriptor_[7] != 64 ||
        deviceDescriptor_[17] != 1 ||
        configurationDescriptor_[0] != 9 ||
        configurationDescriptor_[1] != TUSB_DESC_CONFIGURATION ||
        configurationDescriptor_[5] != 1) {
        return false;
    }
    const uint16_t totalLength =
        static_cast<uint16_t>(configurationDescriptor_[2]) |
        (static_cast<uint16_t>(configurationDescriptor_[3]) << 8);
    if (totalLength != configurationDescriptorBytes_) {
        return false;
    }

    uint8_t currentInterface = 0xFF;
    uint8_t currentInterfaceClass = 0;
    uint8_t endpointCount = 0;
    uint16_t totalHidLength = 0;
    for (uint16_t offset = 0; offset < totalLength;) {
        const uint8_t length = configurationDescriptor_[offset];
        if (length < 2 || offset + length > totalLength) {
            return false;
        }
        const uint8_t type = configurationDescriptor_[offset + 1];
        if (type == TUSB_DESC_INTERFACE && length >= 9) {
            currentInterface = configurationDescriptor_[offset + 2];
            currentInterfaceClass = configurationDescriptor_[offset + 5];
            // Alternate-setting and isochronous topologies require SOF-locked
            // scheduling and are intentionally outside this proxy revision.
            if (configurationDescriptor_[offset + 3] != 0) {
                return false;
            }
        } else if (type == TUSB_DESC_ENDPOINT && length >= 7) {
            const uint8_t address = configurationDescriptor_[offset + 2];
            const uint8_t number = address & 0x0F;
            const uint8_t transferType = configurationDescriptor_[offset + 3] & 0x03;
            const uint16_t packetSize =
                static_cast<uint16_t>(configurationDescriptor_[offset + 4]) |
                (static_cast<uint16_t>(configurationDescriptor_[offset + 5]) << 8);
            if (number == 0 || number >= kEndpointSlots ||
                transferType == TUSB_XFER_ISOCHRONOUS ||
                packetSize == 0 || packetSize > kMaxPacketSize) {
                return false;
            }
            ++endpointCount;
        } else if (type == HID_DESC_TYPE_HID && length >= 9 &&
                   currentInterface != 0xFF &&
                   currentInterfaceClass == TUSB_CLASS_HID) {
            const uint16_t reportLength =
                static_cast<uint16_t>(configurationDescriptor_[offset + 7]) |
                (static_cast<uint16_t>(configurationDescriptor_[offset + 8]) << 8);
            if (reportLength == 0 ||
                reportLength > makcm::bridge::kMaxReportDescriptorSize) {
                return false;
            }
            totalHidLength += reportLength;
            bool found = false;
            for (uint8_t i = 0; i < hidDescriptorCount_; ++i) {
                HidDescriptorState &slot = hidDescriptors_[i];
                if (slot.interfaceNumber == currentInterface) {
                    slot.expectedLength = reportLength;
                    found = slot.receivedLength == reportLength;
                    break;
                }
            }
            if (!found) {
                return false;
            }
        }
        offset += length;
    }
    return endpointCount == profile_.endpointCount &&
           totalHidLength == profile_.reportDescriptorLength;
}

bool PassthroughDevice::finishProfile() {
    topologySupported_ = validateConfiguration() && validateStringDescriptors();
    profileReady_ = topologySupported_;
    if (profileReady_) {
        discoverMouseEndpoint();
    }
    return profileReady_;
}

bool PassthroughDevice::beginUsb() {
    if (!profileReady_) {
        return false;
    }
    gPassthrough = this;
    bool queuesReady = true;
    for (uint8_t i = 1; i < kEndpointSlots; ++i) {
        if (inputQueues_[i] == nullptr) {
            inputQueues_[i] = xQueueCreate(kEndpointQueueDepth,
                                           sizeof(InputItem));
        }
        queuesReady = queuesReady && inputQueues_[i] != nullptr;
    }
    if (outputQueue_ == nullptr) {
        outputQueue_ = xQueueCreate(kEndpointQueueDepth, sizeof(OutputItem));
    }
    queuesReady = queuesReady && outputQueue_ != nullptr;
    if (controlMutex_ == nullptr) {
        controlMutex_ = xSemaphoreCreateMutex();
    }
    if (controlDone_ == nullptr) {
        controlDone_ = xSemaphoreCreateBinary();
    }
    if (inputTaskHandle_ == nullptr && queuesReady) {
        // Bridge parsing runs at priority 6. The USB dispatcher must be able to
        // consume each just-enqueued endpoint item before another UART burst,
        // while remaining below TinyUSB's internal device task.
        xTaskCreate(inputTaskEntry, "UsbProxyTx", 4096, this, 7,
                    &inputTaskHandle_);
    }
    if (outputTaskHandle_ == nullptr && queuesReady) {
        xTaskCreate(outputTaskEntry, "UsbProxyOut", 3072, this, 4,
                    &outputTaskHandle_);
    }
    usbStarted_ = queuesReady && inputTaskHandle_ != nullptr &&
        outputTaskHandle_ != nullptr;
    return usbStarted_;
}

void PassthroughDevice::disconnectUsb() {
    if (usbStarted_) {
        tud_disconnect();
    }
}

void PassthroughDevice::reconnectUsb() {
    if (usbStarted_ && profileReady_) {
        tud_connect();
    }
}

bool PassthroughDevice::queueEndpointInput(uint8_t endpoint,
                                          const uint8_t *data,
                                          uint16_t length) {
    ++inputReceived_;
    const uint8_t number = endpoint & 0x0F;
    uint32_t epoch = 0;
    uint16_t packetSize = 0;
    bool endpointPresent = false;
    if (number != 0 && number < kEndpointSlots) {
        portENTER_CRITICAL(&schedulerMux_);
        epoch = schedulerEpoch_;
        endpointPresent = endpoints_[number].inPresent;
        packetSize = endpoints_[number].inPacketSize;
        portEXIT_CRITICAL(&schedulerMux_);
    }
    if (!usbStarted_ || data == nullptr ||
        (endpoint & 0x80) == 0 || number == 0 || number >= kEndpointSlots ||
        inputQueues_[number] == nullptr ||
        !endpointPresent || length == 0 || length > packetSize) {
        ++inputQueueDrops_;
        return false;
    }
    InputItem item = {};
    item.epoch = epoch;
    item.length = length;
    memcpy(item.data, data, length);

    // XInput IN reports describe the complete current controller state; they
    // are not relative events. Mature XInput device implementations send the
    // latest state when the endpoint becomes ready instead of replaying every
    // sample accumulated while it was busy. Replaying a 16-frame tail makes
    // sticks/triggers feel frozen as soon as another IN endpoint consumes a
    // little scheduling time. Relative mouse reports deliberately retain FIFO
    // semantics because dropping one would lose movement.
    uint32_t coalesced = 0;
    if (endpoint == xinputInEndpoint_) {
        InputItem obsolete = {};
        while (xQueueReceive(inputQueues_[number], &obsolete, 0) == pdTRUE) {
            ++coalesced;
        }
    }

    if (xQueueSend(inputQueues_[number], &item, 0) == pdTRUE) {
        ++inputQueued_;
        const UBaseType_t depth = uxQueueMessagesWaiting(inputQueues_[number]);
        portENTER_CRITICAL(&schedulerMux_);
        if (epoch == schedulerEpoch_) {
            EndpointState &state = endpoints_[number];
            ++state.queued;
            state.queueCoalesced += coalesced;
            if (depth > state.queueHighWater) {
                state.queueHighWater = static_cast<uint8_t>(depth);
            }
            if (endpoint == mouseInEndpoint_ &&
                makcm::hid::matchesReport(mouseLayout_, data, length)) {
                portENTER_CRITICAL(&mouseMux_);
                memcpy(lastMouseInput_, data, length);
                lastMouseInputLength_ = length;
                portEXIT_CRITICAL(&mouseMux_);
            }
        }
        portEXIT_CRITICAL(&schedulerMux_);
        notifyInputTask();
        return true;
    }
    ++inputQueueDrops_;
    portENTER_CRITICAL(&schedulerMux_);
    if (epoch == schedulerEpoch_) ++endpoints_[number].queueDrops;
    portEXIT_CRITICAL(&schedulerMux_);
    return false;
}

bool PassthroughDevice::queueInput(makcm::bridge::MessageType,
                                   const uint8_t *data, uint16_t length) {
    return queueEndpointInput(profile_.inEndpoint, data, length);
}

void PassthroughDevice::inputTaskEntry(void *argument) {
    static_cast<PassthroughDevice *>(argument)->inputTask();
}

void PassthroughDevice::outputTaskEntry(void *argument) {
    static_cast<PassthroughDevice *>(argument)->outputTask();
}

void PassthroughDevice::outputTask() {
    OutputItem item = {};
    while (true) {
        if (xQueueReceive(outputQueue_, &item, portMAX_DELAY) == pdTRUE) {
            portENTER_CRITICAL(&schedulerMux_);
            const bool current = item.epoch == schedulerEpoch_;
            portEXIT_CRITICAL(&schedulerMux_);
            if (!current) {
                ++endpointOutputDrops_;
                continue;
            }
            uint8_t payload[kMaxPacketSize + 1] = {};
            payload[0] = item.endpoint;
            memcpy(payload + 1, item.data, item.length);
            if (!sendBridgeFrameForSession(
                    makcm::bridge::MessageType::EndpointOut, payload,
                    static_cast<uint16_t>(item.length + 1),
                    item.bridgeSession)) {
                ++endpointSendErrors_;
            }
        }
    }
}

void PassthroughDevice::inputTask() {
    while (true) {
        const TickType_t wait = schedulerWorkPending()
            ? pdMS_TO_TICKS(1) : portMAX_DELAY;
        ulTaskNotifyTake(pdTRUE, wait);
        // Every endpoint owns its own FIFO and transfer phase. A busy mouse
        // endpoint can never wait or spin in front of XInput/vendor raw IN.
        for (uint8_t number = 1; number < kEndpointSlots; ++number) {
            pumpEndpoint(number);
            pumpOutputEndpoint(number);
        }
    }
}

void PassthroughDevice::notifyInputTask() {
    if (inputTaskHandle_ != nullptr) xTaskNotifyGive(inputTaskHandle_);
}

bool PassthroughDevice::schedulerWorkPending() const {
    if (!usbStarted_) return false;
    for (uint8_t number = 1; number < kEndpointSlots; ++number) {
        bool canStart = false;
        bool retry = false;
        bool outputRearm = false;
        portENTER_CRITICAL(&schedulerMux_);
        const EndpointState &state = endpoints_[number];
        canStart = state.inPresent &&
            state.inTransfer.phase() == EndpointPhase::Idle;
        retry = state.retryInValid;
        outputRearm = state.outPresent && state.outNeedsRearm;
        portEXIT_CRITICAL(&schedulerMux_);
        if (outputRearm ||
            (canStart && (retry ||
             (inputQueues_[number] != nullptr &&
              uxQueueMessagesWaiting(inputQueues_[number]) != 0)))) {
            return true;
        }
    }
    const uint8_t mouseNumber = mouseInEndpoint_ & 0x0F;
    if (mouseNumber != 0 && mouseNumber < kEndpointSlots) {
        bool mouseIdle = false;
        portENTER_CRITICAL(&schedulerMux_);
        mouseIdle = endpoints_[mouseNumber].inPresent &&
            endpoints_[mouseNumber].inTransfer.phase() == EndpointPhase::Idle;
        portEXIT_CRITICAL(&schedulerMux_);
        if (mouseIdle && mouseInjectionPending()) return true;
    }
    return false;
}

bool PassthroughDevice::prepareMousePhysical(
    const InputItem &item, uint8_t *output, uint16_t &length,
    makcm::hid::MouseMergeDelta &consumed,
    uint32_t &buttonGeneration) {
    if (!makcm::hid::matchesReport(mouseLayout_, item.data, item.length)) {
        return false;
    }
    makcm::hid::MouseInjectionSnapshot snapshot;
    portENTER_CRITICAL(&mouseMux_);
    snapshot = mouseLedger_.snapshot();
    buttonGeneration = snapshot.buttonGeneration;
    const bool needsMerge = snapshot.buttons != 0 ||
        snapshot.buttonGeneration != snapshot.buttonSentGeneration ||
        snapshot.x != 0 || snapshot.y != 0 || snapshot.wheel != 0 ||
        snapshot.pan != 0;
    if (needsMerge) {
        length = makcm::hid::mergeMouseReport(
            mouseLayout_, item.data, item.length, true, snapshot.buttons,
            snapshot.x, snapshot.y, snapshot.wheel, snapshot.pan,
            output, kMaxPacketSize, &consumed);
        if (length != 0 &&
            !mouseLedger_.reserve(consumed, buttonGeneration)) {
            length = 0;
        }
    }
    portEXIT_CRITICAL(&mouseMux_);
    return needsMerge && length != 0;
}

bool PassthroughDevice::prepareMouseSynthetic(
    uint8_t *output, uint16_t &length,
    makcm::hid::MouseMergeDelta &consumed,
    uint32_t &buttonGeneration) {
    makcm::hid::MouseInjectionSnapshot snapshot;
    alignas(4) uint8_t base[kMaxPacketSize] = {};
    uint16_t baseLength;
    portENTER_CRITICAL(&mouseMux_);
    snapshot = mouseLedger_.snapshot();
    baseLength = lastMouseInputLength_;
    if (baseLength != 0) memcpy(base, lastMouseInput_, baseLength);
    buttonGeneration = snapshot.buttonGeneration;
    if (snapshot.buttonGeneration == snapshot.buttonSentGeneration &&
        snapshot.x == 0 && snapshot.y == 0 && snapshot.wheel == 0 &&
        snapshot.pan == 0) {
        portEXIT_CRITICAL(&mouseMux_);
        return false;
    }
    length = makcm::hid::mergeMouseReport(
        mouseLayout_, baseLength == 0 ? nullptr : base, baseLength,
        false, snapshot.buttons, snapshot.x, snapshot.y, snapshot.wheel,
        snapshot.pan,
        output, kMaxPacketSize, &consumed);
    if (length != 0 && !mouseLedger_.reserve(consumed, buttonGeneration)) {
        length = 0;
    }
    portEXIT_CRITICAL(&mouseMux_);
    return length != 0;
}

void PassthroughDevice::settleMouseInjection(bool success) {
    portENTER_CRITICAL(&mouseMux_);
    mouseLedger_.settle(success);
    portEXIT_CRITICAL(&mouseMux_);
}

bool PassthroughDevice::mouseInjectionPending() const {
    portENTER_CRITICAL(&mouseMux_);
    const bool pending = mouseLedger_.pending();
    portEXIT_CRITICAL(&mouseMux_);
    return pending;
}

bool PassthroughDevice::mouseMovementPending() const {
    portENTER_CRITICAL(&mouseMux_);
    const makcm::hid::MouseInjectionSnapshot snapshot =
        mouseLedger_.snapshot();
    const bool pending = snapshot.x != 0 || snapshot.y != 0 ||
        snapshot.wheel != 0 || snapshot.pan != 0;
    portEXIT_CRITICAL(&mouseMux_);
    return pending;
}

bool PassthroughDevice::mouseInjectionAllowed(uint64_t nowUs) const {
    portENTER_CRITICAL(&mouseMux_);
    const bool allowed = mouseRateGate_.allowed(nowUs);
    portEXIT_CRITICAL(&mouseMux_);
    return allowed;
}

void PassthroughDevice::noteMouseInjectionSubmit(uint64_t nowUs) {
    portENTER_CRITICAL(&mouseMux_);
    mouseRateGate_.noteSubmit(nowUs);
    portEXIT_CRITICAL(&mouseMux_);
}

bool PassthroughDevice::pumpEndpoint(uint8_t number) {
    if (!usbStarted_ || !tud_mounted() || number == 0 ||
        number >= kEndpointSlots || inputQueues_[number] == nullptr) {
        return false;
    }

    uint32_t epoch = 0;
    uint16_t packetSize = 0;
    bool forceSynthetic = false;
    portENTER_CRITICAL(&schedulerMux_);
    EndpointState &state = endpoints_[number];
    if (!state.inPresent || !state.inTransfer.begin(schedulerEpoch_)) {
        portEXIT_CRITICAL(&schedulerMux_);
        return false;
    }
    epoch = state.inTransfer.epoch();
    packetSize = state.inPacketSize;
    forceSynthetic = state.forceSyntheticNext;
    portEXIT_CRITICAL(&schedulerMux_);

    const uint8_t endpoint = static_cast<uint8_t>(0x80 | number);
    const uint64_t nowUs = static_cast<uint64_t>(esp_timer_get_time());
    const bool allowMouseInjection = endpoint != mouseInEndpoint_ ||
        mouseInjectionAllowed(nowUs);
    if (!usbd_edpt_claim(0, endpoint)) {
        portENTER_CRITICAL(&schedulerMux_);
        if (epoch == schedulerEpoch_) state.inTransfer.cancelStarting(epoch);
        portEXIT_CRITICAL(&schedulerMux_);
        ++endpointRetryPolls_;
        return false;
    }

    InputItem item = {};
    bool physical = false;
    bool synthetic = false;
    bool merged = false;
    uint16_t length = 0;
    makcm::hid::MouseMergeDelta consumed;
    uint32_t buttonGeneration = 0;
    alignas(4) uint8_t prepared[kMaxPacketSize] = {};

    // If a physical report was pinned at its logical limit and could not carry
    // any same-direction injection, give one host poll to the relative ledger
    // before returning to the physical FIFO. This is per mouse endpoint only;
    // controller/XInput endpoints remain completely independent.
    if (forceSynthetic && allowMouseInjection &&
        endpoint == mouseInEndpoint_ && mouseLayout_.valid) {
        synthetic = prepareMouseSynthetic(prepared, length, consumed,
                                          buttonGeneration);
        if (!synthetic) {
            portENTER_CRITICAL(&schedulerMux_);
            if (epoch == schedulerEpoch_) state.forceSyntheticNext = false;
            portEXIT_CRITICAL(&schedulerMux_);
        }
    }

    if (!synthetic) {
        portENTER_CRITICAL(&schedulerMux_);
        if (epoch == schedulerEpoch_ &&
            state.inTransfer.phase() == EndpointPhase::Starting &&
            state.retryInValid) {
            item.epoch = epoch;
            item.length = state.retryInLength;
            memcpy(item.data, state.retryIn, item.length);
            state.retryInValid = false;
            physical = true;
        }
        portEXIT_CRITICAL(&schedulerMux_);
        while (!physical &&
               xQueueReceive(inputQueues_[number], &item, 0) == pdTRUE) {
            if (item.epoch == epoch) {
                physical = true;
            } else {
                ++endpointStaleInputs_;
            }
        }
    }

    if (physical) {
        length = item.length;
        memcpy(prepared, item.data, item.length);
        if (endpoint == mouseInEndpoint_ && allowMouseInjection) {
            merged = prepareMousePhysical(item, prepared, length,
                                          consumed, buttonGeneration);
        }
    } else if (!synthetic && allowMouseInjection &&
               endpoint == mouseInEndpoint_ &&
               mouseLayout_.valid) {
        synthetic = prepareMouseSynthetic(prepared, length, consumed,
                                           buttonGeneration);
        if (!synthetic) {
            usbd_edpt_release(0, endpoint);
            portENTER_CRITICAL(&schedulerMux_);
            if (epoch == schedulerEpoch_)
                state.inTransfer.cancelStarting(epoch);
            portEXIT_CRITICAL(&schedulerMux_);
            return false;
        }
    } else {
        usbd_edpt_release(0, endpoint);
        portENTER_CRITICAL(&schedulerMux_);
        if (epoch == schedulerEpoch_)
            state.inTransfer.cancelStarting(epoch);
        portEXIT_CRITICAL(&schedulerMux_);
        return false;
    }
    if (synthetic) ++syntheticComposed_;

    ++endpointSendAttempts_;
    if (length == 0 || length > packetSize) {
        usbd_edpt_release(0, endpoint);
        ++endpointSendErrors_;
        portENTER_CRITICAL(&schedulerMux_);
        if (epoch == schedulerEpoch_ &&
            state.inTransfer.phase() == EndpointPhase::Starting) {
            if (physical) {
                memcpy(state.retryIn, item.data, item.length);
                state.retryInLength = item.length;
                state.retryInValid = true;
            }
            state.inTransfer.cancelStarting(epoch);
        }
        portEXIT_CRITICAL(&schedulerMux_);
        if (merged || synthetic) settleMouseInjection(false);
        return false;
    }

    const bool zeroMovementProgress = merged &&
        consumed.x == 0 && consumed.y == 0 && consumed.wheel == 0 &&
        consumed.pan == 0 && mouseMovementPending();
    portENTER_CRITICAL(&schedulerMux_);
    if (epoch != schedulerEpoch_ ||
        state.inTransfer.phase() != EndpointPhase::Starting) {
        portEXIT_CRITICAL(&schedulerMux_);
        usbd_edpt_release(0, endpoint);
        if (merged || synthetic) settleMouseInjection(false);
        return false;
    }
    memcpy(state.activeIn, prepared, length);
    state.activeInPhysical = physical;
    state.activeInSynthetic = synthetic;
    state.activeInMouseMerged = merged;
    state.activeNeedsStandalone = zeroMovementProgress;
    state.activeOriginalLength = physical ? item.length : 0;
    if (physical) memcpy(state.activeOriginal, item.data, item.length);
    if (!state.inTransfer.arm(epoch, length)) {
        portEXIT_CRITICAL(&schedulerMux_);
        usbd_edpt_release(0, endpoint);
        if (merged || synthetic) settleMouseInjection(false);
        return false;
    }
    portEXIT_CRITICAL(&schedulerMux_);

    const bool submitted =
        usbd_edpt_xfer(0, endpoint, state.activeIn, length);
    // Match TinyUSB's XInput/HID class-driver contract: the claim protects the
    // submit operation, while the busy flag owns the transfer until its
    // completion callback. Holding the claim for the whole USB transaction is
    // unnecessary and differs from the maintained reference drivers.
    usbd_edpt_release(0, endpoint);
    if (!submitted) {
        ++endpointSendErrors_;
        portENTER_CRITICAL(&schedulerMux_);
        if (epoch == schedulerEpoch_ && state.inTransfer.submitFailed(epoch)) {
            if (physical) {
                memcpy(state.retryIn, item.data, item.length);
                state.retryInLength = item.length;
                state.retryInValid = true;
            }
            state.activeInPhysical = false;
            state.activeInSynthetic = false;
            state.activeInMouseMerged = false;
            state.activeNeedsStandalone = false;
            state.activeOriginalLength = 0;
        }
        portEXIT_CRITICAL(&schedulerMux_);
        if (merged || synthetic) settleMouseInjection(false);
        ++endpointRetryPolls_;
        return false;
    }
    portENTER_CRITICAL(&schedulerMux_);
    if (epoch == schedulerEpoch_) ++state.transfersStarted;
    portEXIT_CRITICAL(&schedulerMux_);
    ++endpointSendSuccess_;
    if (merged) ++syntheticMerged_;
    if (synthetic) ++syntheticTransfersStarted_;
    if (merged || synthetic) noteMouseInjectionSubmit(nowUs);
    return true;
}

bool PassthroughDevice::pumpOutputEndpoint(uint8_t number) {
    if (!usbStarted_ || !tud_mounted() || number == 0 ||
        number >= kEndpointSlots) {
        return false;
    }
    uint32_t epoch = 0;
    uint16_t packetSize = 0;
    portENTER_CRITICAL(&schedulerMux_);
    EndpointState &state = endpoints_[number];
    if (!state.outPresent || !state.outNeedsRearm ||
        !state.outTransfer.begin(schedulerEpoch_)) {
        portEXIT_CRITICAL(&schedulerMux_);
        return false;
    }
    epoch = state.outTransfer.epoch();
    packetSize = state.outPacketSize;
    state.outNeedsRearm = false;
    portEXIT_CRITICAL(&schedulerMux_);

    const uint8_t endpoint = number;
    if (!usbd_edpt_claim(0, endpoint)) {
        portENTER_CRITICAL(&schedulerMux_);
        if (epoch == schedulerEpoch_ && state.outPresent &&
            state.outTransfer.cancelStarting(epoch)) {
            state.outNeedsRearm = true;
        }
        portEXIT_CRITICAL(&schedulerMux_);
        ++endpointRetryPolls_;
        return false;
    }
    portENTER_CRITICAL(&schedulerMux_);
    if (epoch != schedulerEpoch_ || !state.outPresent ||
        state.outTransfer.phase() != EndpointPhase::Starting) {
        portEXIT_CRITICAL(&schedulerMux_);
        usbd_edpt_release(0, endpoint);
        return false;
    }
    if (!state.outTransfer.arm(epoch, packetSize)) {
        portEXIT_CRITICAL(&schedulerMux_);
        usbd_edpt_release(0, endpoint);
        return false;
    }
    portEXIT_CRITICAL(&schedulerMux_);
    const bool submitted =
        usbd_edpt_xfer(0, endpoint, state.outBuffer, packetSize);
    usbd_edpt_release(0, endpoint);
    if (!submitted) {
        portENTER_CRITICAL(&schedulerMux_);
        if (epoch == schedulerEpoch_ &&
            state.outTransfer.submitFailed(epoch)) {
            state.outNeedsRearm = true;
        }
        portEXIT_CRITICAL(&schedulerMux_);
        ++endpointRetryPolls_;
        return false;
    }
    return true;
}

bool PassthroughDevice::inputEndpointReady() {
    return usbStarted_ && tud_mounted();
}

void PassthroughDevice::printStatus(Print &out) {
    makcm::hid::MouseInjectionSnapshot snapshot;
    uint16_t lastMouseLength;
    bool reservationActive;
    bool injectionEnabled;
    uint16_t injectionRateHz;
    uint32_t syntheticSuppressed;
    portENTER_CRITICAL(&mouseMux_);
    snapshot = mouseLedger_.snapshot();
    reservationActive = mouseLedger_.reservationActive();
    lastMouseLength = lastMouseInputLength_;
    injectionEnabled = mouseRateGate_.enabled();
    injectionRateHz = mouseRateGate_.rateHz();
    syntheticSuppressed = syntheticSuppressed_;
    portEXIT_CRITICAL(&mouseMux_);
    const uint8_t mouseNumber = mouseInEndpoint_ & 0x0F;
    const uint8_t xinputNumber = xinputInEndpoint_ & 0x0F;
    const unsigned mouseDepth = mouseNumber != 0 &&
            inputQueues_[mouseNumber] != nullptr
        ? uxQueueMessagesWaiting(inputQueues_[mouseNumber]) : 0;
    const unsigned xinputDepth = xinputNumber != 0 &&
            inputQueues_[xinputNumber] != nullptr
        ? uxQueueMessagesWaiting(inputQueues_[xinputNumber]) : 0;
    uint8_t mouseHighWater = 0;
    uint8_t xinputHighWater = 0;
    uint8_t mousePhase = 0;
    uint8_t xinputPhase = 0;
    uint32_t mouseDrops = 0;
    uint32_t xinputDrops = 0;
    uint32_t xinputCoalesced = 0;
    uint32_t mouseStarted = 0;
    uint32_t mouseCompleted = 0;
    uint32_t xinputStarted = 0;
    uint32_t xinputCompleted = 0;
    uint32_t epoch = 0;
    portENTER_CRITICAL(&schedulerMux_);
    epoch = schedulerEpoch_;
    if (mouseNumber != 0) {
        mouseHighWater = endpoints_[mouseNumber].queueHighWater;
        mouseDrops = endpoints_[mouseNumber].queueDrops;
        mousePhase = static_cast<uint8_t>(
            endpoints_[mouseNumber].inTransfer.phase());
        mouseStarted = endpoints_[mouseNumber].transfersStarted;
        mouseCompleted = endpoints_[mouseNumber].transfersCompleted;
    }
    if (xinputNumber != 0) {
        xinputHighWater = endpoints_[xinputNumber].queueHighWater;
        xinputDrops = endpoints_[xinputNumber].queueDrops;
        xinputCoalesced = endpoints_[xinputNumber].queueCoalesced;
        xinputPhase = static_cast<uint8_t>(
            endpoints_[xinputNumber].inTransfer.phase());
        xinputStarted = endpoints_[xinputNumber].transfersStarted;
        xinputCompleted = endpoints_[xinputNumber].transfersCompleted;
    }
    portEXIT_CRITICAL(&schedulerMux_);
    out.printf(
        "BRIDGE session=%08lx state=%u profile=%u usb=%u mounted=%u "
        "epoch=%lu kind=%u raw=%u/%u+%u/%u eps=%u rx=%lu queued=%lu drop=%lu "
        "attempts=%lu sent=%lu error=%lu short=%lu stale=%lu/%lu "
        "retry=%lu outdrop=%lu mouse=%02x/%u/%u/%u/%u "
        "pend=%lld/%lld/%lld/%lld btn=%02x/%lu/%lu/%u "
        "synth=%lu/%lu/%lu/%lu/%lu/%lu/%lu "
        "xq=%u/%u/%lu/%u xc=%lu mq=%u/%u/%lu/%u "
        "xx=%lu/%lu mx=%lu/%lu inj=%u/%u/%lu "
        "ctrl=%lu/%lu ctrlerr=%lu\r\n",
        static_cast<unsigned long>(bridgeActiveSession()),
        static_cast<unsigned>(bridgeProxyState()), profileReady_ ? 1 : 0,
        usbStarted_ ? 1 : 0, tud_mounted() ? 1 : 0,
        static_cast<unsigned long>(epoch),
        static_cast<unsigned>(profile_.kind), deviceDescriptorBytes_,
        profile_.deviceDescriptorLength, configurationDescriptorBytes_,
        profile_.configurationDescriptorLength, profile_.endpointCount,
        static_cast<unsigned long>(inputReceived_),
        static_cast<unsigned long>(inputQueued_),
        static_cast<unsigned long>(inputQueueDrops_),
        static_cast<unsigned long>(endpointSendAttempts_),
        static_cast<unsigned long>(endpointSendSuccess_),
        static_cast<unsigned long>(endpointSendErrors_),
        static_cast<unsigned long>(endpointShortTransfers_),
        static_cast<unsigned long>(endpointStaleCompletions_),
        static_cast<unsigned long>(endpointStaleInputs_),
        static_cast<unsigned long>(endpointRetryPolls_),
        static_cast<unsigned long>(endpointOutputDrops_),
        mouseInEndpoint_, mouseLayout_.valid ? 1 : 0,
        mouseLayout_.hasReportId ? mouseLayout_.reportId : 0,
        mouseLayout_.reportLength, lastMouseLength,
        static_cast<long long>(snapshot.x),
        static_cast<long long>(snapshot.y),
        static_cast<long long>(snapshot.wheel),
        static_cast<long long>(snapshot.pan), snapshot.buttons,
        static_cast<unsigned long>(snapshot.buttonGeneration),
        static_cast<unsigned long>(snapshot.buttonSentGeneration),
        reservationActive ? 1 : 0,
        static_cast<unsigned long>(syntheticRequests_),
        static_cast<unsigned long>(syntheticComposed_),
        static_cast<unsigned long>(syntheticQueued_),
        static_cast<unsigned long>(syntheticQueueErrors_),
        static_cast<unsigned long>(syntheticMerged_),
        static_cast<unsigned long>(syntheticTransfersStarted_),
        static_cast<unsigned long>(syntheticTransfersCompleted_),
        xinputDepth, xinputHighWater,
        static_cast<unsigned long>(xinputDrops), xinputPhase,
        static_cast<unsigned long>(xinputCoalesced),
        mouseDepth, mouseHighWater,
        static_cast<unsigned long>(mouseDrops), mousePhase,
        static_cast<unsigned long>(xinputStarted),
        static_cast<unsigned long>(xinputCompleted),
        static_cast<unsigned long>(mouseStarted),
        static_cast<unsigned long>(mouseCompleted),
        injectionEnabled ? 1 : 0,
        static_cast<unsigned>(injectionRateHz),
        static_cast<unsigned long>(syntheticSuppressed),
        static_cast<unsigned long>(controlRequests_),
        static_cast<unsigned long>(controlOneWay_),
        static_cast<unsigned long>(controlErrors_));
}

const uint16_t *PassthroughDevice::stringDescriptor(uint8_t index,
                                                    uint16_t langid) const {
    for (uint8_t i = 0; i < stringDescriptorCount_; ++i) {
        const StringDescriptorState &slot = stringDescriptors_[i];
        if (slot.index == index &&
            (index == 0 || slot.langid == langid || langid == 0) &&
            slot.receivedLength >= 2 && slot.data[0] == slot.receivedLength) {
            return reinterpret_cast<const uint16_t *>(slot.data);
        }
    }
    // Raw string descriptors are authoritative.  Returning a synthesized
    // ASCII fallback for an unsupported LANGID creates an observable identity
    // that the physical device never exposed.
    return nullptr;
}

void PassthroughDevice::driverInit() {
    driverReset(0);
}

void PassthroughDevice::driverReset(uint8_t) {
    portENTER_CRITICAL(&schedulerMux_);
    if (++schedulerEpoch_ == 0) ++schedulerEpoch_;
    memset(endpoints_, 0, sizeof(endpoints_));
    portENTER_CRITICAL(&mouseMux_);
    // A USB bus reset does not end the physical-device session. Keep accepted
    // relative movement and the button latch, but make held buttons dirty so
    // the newly enumerated host receives their state again.
    mouseLedger_.markHostReset();
    lastMouseInputLength_ = 0;
    memset(lastMouseInput_, 0, sizeof(lastMouseInput_));
    portEXIT_CRITICAL(&mouseMux_);
    portEXIT_CRITICAL(&schedulerMux_);
    for (uint8_t i = 1; i < kEndpointSlots; ++i) {
        if (inputQueues_[i] != nullptr) xQueueReset(inputQueues_[i]);
    }
    if (outputQueue_ != nullptr) xQueueReset(outputQueue_);
}

uint16_t PassthroughDevice::driverOpen(uint8_t rhport,
                                       const uint8_t *interfaceDescriptor,
                                       uint16_t maxLength) {
    if (!profileReady_ || interfaceDescriptor == nullptr || maxLength < 9 ||
        interfaceDescriptor[1] != TUSB_DESC_INTERFACE) {
        return 0;
    }

    uint8_t associatedInterfaces = 1;
    const uint8_t *base = configurationDescriptor_;
    const uint8_t *end = base + configurationDescriptorBytes_;
    const uint8_t *previous = nullptr;
    for (const uint8_t *cursor = base; cursor < end;) {
        if (cursor == interfaceDescriptor) {
            if (previous != nullptr &&
                previous[1] == TUSB_DESC_INTERFACE_ASSOCIATION &&
                previous[0] >= 8) {
                associatedInterfaces = previous[4];
            }
            break;
        }
        previous = cursor;
        if (cursor[0] < 2) {
            return 0;
        }
        cursor += cursor[0];
    }

    const uint8_t *cursor = interfaceDescriptor;
    const uint8_t *limit = std::min(end, interfaceDescriptor + maxLength);
    uint8_t interfacesSeen = 0;
    while (cursor < limit) {
        const uint8_t length = cursor[0];
        if (length < 2 || cursor + length > limit) {
            return 0;
        }
        const uint8_t type = cursor[1];
        if (type == TUSB_DESC_INTERFACE) {
            if (interfacesSeen >= associatedInterfaces) {
                break;
            }
            ++interfacesSeen;
        } else if (type == TUSB_DESC_ENDPOINT && length >= 7) {
            const auto *endpoint =
                reinterpret_cast<const tusb_desc_endpoint_t *>(cursor);
            const uint8_t address = endpoint->bEndpointAddress;
            const uint8_t number = address & 0x0F;
            if (number == 0 || number >= kEndpointSlots ||
                !usbd_edpt_open(rhport, endpoint)) {
                return 0;
            }
            const uint16_t packetSize = endpoint->wMaxPacketSize;
            uint32_t epoch = 0;
            portENTER_CRITICAL(&schedulerMux_);
            epoch = schedulerEpoch_;
            EndpointState &state = endpoints_[number];
            if ((address & 0x80) != 0) {
                state.inPresent = true;
                state.inPacketSize = packetSize;
                state.inTransfer.open(epoch);
                portEXIT_CRITICAL(&schedulerMux_);
            } else {
                state.outPresent = true;
                state.outPacketSize = packetSize;
                state.outTransfer.open(epoch);
                state.outTransfer.begin(epoch);
                state.outTransfer.arm(epoch, packetSize);
                portEXIT_CRITICAL(&schedulerMux_);
                if (!usbd_edpt_xfer(rhport, address,
                                    endpoints_[number].outBuffer,
                                    packetSize)) {
                    portENTER_CRITICAL(&schedulerMux_);
                    if (epoch == schedulerEpoch_ &&
                        endpoints_[number].outTransfer.submitFailed(epoch)) {
                        endpoints_[number].outNeedsRearm = true;
                    }
                    portEXIT_CRITICAL(&schedulerMux_);
                    notifyInputTask();
                }
            }
        }
        cursor += length;
    }
    notifyInputTask();
    return static_cast<uint16_t>(cursor - interfaceDescriptor);
}

const PassthroughDevice::HidDescriptorState *
PassthroughDevice::findHidDescriptor(uint8_t interfaceNumber) const {
    for (uint8_t i = 0; i < hidDescriptorCount_; ++i) {
        if (hidDescriptors_[i].interfaceNumber == interfaceNumber) {
            return &hidDescriptors_[i];
        }
    }
    return nullptr;
}

bool PassthroughDevice::driverControl(
    uint8_t rhport, uint8_t stage,
    const tusb_control_request_t *request) {
    if (request == nullptr || !profileReady_) {
        return false;
    }
    if (stage == CONTROL_STAGE_DATA) {
        if (pendingOutRequestValid_) {
            const bool ok = forwardControlOneWay(
                pendingOutRequest_, controlOutBuffer_,
                pendingOutRequest_.wLength);
            pendingOutRequestValid_ = false;
            return ok;
        }
        return true;
    }
    if (stage == CONTROL_STAGE_ACK) return true;
    if (stage != CONTROL_STAGE_SETUP) {
        return true;
    }
    // A new SETUP cancels any incomplete older control OUT transaction.
    pendingOutRequestValid_ = false;

    // HID report descriptors are part of the stable profile and can be
    // returned without a UART round trip during enumeration.
    if (request->bmRequestType_bit.type == TUSB_REQ_TYPE_STANDARD &&
        request->bmRequestType_bit.recipient == TUSB_REQ_RCPT_INTERFACE &&
        request->bRequest == TUSB_REQ_GET_DESCRIPTOR &&
        static_cast<uint8_t>(request->wValue >> 8) == HID_DESC_TYPE_REPORT) {
        const HidDescriptorState *descriptor =
            findHidDescriptor(static_cast<uint8_t>(request->wIndex));
        return descriptor != nullptr &&
               tud_control_xfer(rhport, request,
                                const_cast<uint8_t *>(descriptor->data),
                                descriptor->receivedLength);
    }

    if (request->bmRequestType_bit.direction == TUSB_DIR_IN) {
        uint16_t responseLength = std::min<uint16_t>(
            request->wLength, sizeof(controlResponse_));
        if (!forwardControl(*request, nullptr, 0, controlResponse_,
                            responseLength)) {
            return false;
        }
        return tud_control_xfer(rhport, request, controlResponse_,
                                responseLength);
    }

    if (request->wLength == 0) {
        return forwardControlOneWay(*request, nullptr, 0) &&
               tud_control_status(rhport, request);
    }
    if (request->wLength > sizeof(controlOutBuffer_)) {
        return false;
    }
    pendingOutRequest_ = *request;
    pendingOutRequestValid_ = true;
    return tud_control_xfer(rhport, request, controlOutBuffer_,
                            request->wLength);
}

bool PassthroughDevice::forwardControlOneWay(
    const tusb_control_request_t &request, const uint8_t *outData,
    uint16_t outLength) {
    if ((request.bmRequestType & 0x80) != 0 ||
        sizeof(makcm::bridge::ControlRequestHeader) + outLength >
            makcm::bridge::kMaxPayloadSize ||
        (outLength != 0 && outData == nullptr)) {
        ++controlErrors_;
        return false;
    }
    uint16_t requestId = nextControlId_++;
    if (requestId == 0) requestId = nextControlId_++;
    uint8_t payload[makcm::bridge::kMaxPayloadSize] = {};
    makcm::bridge::ControlRequestHeader header;
    header.requestId = requestId;
    header.bmRequestType = request.bmRequestType;
    header.bRequest = request.bRequest;
    header.wValue = request.wValue;
    header.wIndex = request.wIndex;
    header.wLength = request.wLength;
    header.dataLength = outLength;
    memcpy(payload, &header, sizeof(header));
    if (outLength != 0) memcpy(payload + sizeof(header), outData, outLength);
    ++controlRequests_;
    ++controlOneWay_;
    if (!sendBridgeFrame(
            makcm::bridge::MessageType::ControlRequest, payload,
            static_cast<uint16_t>(sizeof(header) + outLength))) {
        ++controlErrors_;
        return false;
    }
    return true;
}

bool PassthroughDevice::forwardControl(const tusb_control_request_t &request,
                                       const uint8_t *outData,
                                       uint16_t outLength, uint8_t *inData,
                                       uint16_t &inLength) {
    if (controlMutex_ == nullptr || controlDone_ == nullptr ||
        sizeof(makcm::bridge::ControlRequestHeader) + outLength >
            makcm::bridge::kMaxPayloadSize ||
        xSemaphoreTake(controlMutex_, pdMS_TO_TICKS(10)) != pdTRUE) {
        ++controlErrors_;
        return false;
    }
    xSemaphoreTake(controlDone_, 0);
    uint16_t requestId = nextControlId_++;
    if (requestId == 0) {
        requestId = nextControlId_++;
    }
    pendingControlId_ = requestId;
    controlStatus_ = makcm::bridge::ControlStatus::Error;
    controlResponseLength_ = 0;

    uint8_t payload[makcm::bridge::kMaxPayloadSize] = {};
    makcm::bridge::ControlRequestHeader header;
    header.requestId = requestId;
    header.bmRequestType = request.bmRequestType;
    header.bRequest = request.bRequest;
    header.wValue = request.wValue;
    header.wIndex = request.wIndex;
    header.wLength = request.wLength;
    header.dataLength = outLength;
    memcpy(payload, &header, sizeof(header));
    if (outLength != 0 && outData != nullptr) {
        memcpy(payload + sizeof(header), outData, outLength);
    }
    ++controlRequests_;
    const bool sent = sendBridgeFrame(
        makcm::bridge::MessageType::ControlRequest, payload,
        sizeof(header) + outLength);
    const bool completed = sent &&
        xSemaphoreTake(controlDone_, pdMS_TO_TICKS(120)) == pdTRUE &&
        controlStatus_ == makcm::bridge::ControlStatus::Completed;
    if (completed && inData != nullptr) {
        const uint16_t copied = std::min(inLength, controlResponseLength_);
        memcpy(inData, controlResponse_, copied);
        inLength = copied;
    } else if (!completed) {
        ++controlErrors_;
    }
    pendingControlId_ = 0;
    xSemaphoreGive(controlMutex_);
    return completed;
}

void PassthroughDevice::acceptControlResponse(const uint8_t *data,
                                              uint16_t length) {
    if (data == nullptr || length < sizeof(makcm::bridge::ControlResponseHeader) ||
        controlDone_ == nullptr) {
        return;
    }
    makcm::bridge::ControlResponseHeader header;
    memcpy(&header, data, sizeof(header));
    if (header.requestId != pendingControlId_ ||
        sizeof(header) + header.dataLength > length ||
        header.dataLength > sizeof(controlResponse_)) {
        return;
    }
    controlStatus_ = header.status;
    controlResponseLength_ = header.dataLength;
    if (header.dataLength != 0) {
        memcpy(controlResponse_, data + sizeof(header), header.dataLength);
    }
    xSemaphoreGive(controlDone_);
}

void PassthroughDevice::acceptFeatureResponse(const uint8_t *data,
                                              uint16_t length) {
    acceptControlResponse(data, length);
}

bool PassthroughDevice::driverTransfer(uint8_t rhport, uint8_t endpoint,
                                       uint8_t result,
                                       uint32_t transferred) {
    (void)rhport;
    const uint8_t number = endpoint & 0x0F;
    if (number == 0 || number >= kEndpointSlots) {
        return false;
    }
    EndpointState &state = endpoints_[number];
    if ((endpoint & 0x80) == 0) {
        OutputItem item = {};
        bool queueOutput = false;
        portENTER_CRITICAL(&schedulerMux_);
        const EndpointCompletion completion =
            state.outTransfer.beginCompletion(
                schedulerEpoch_, result == XFER_RESULT_SUCCESS,
                transferred, false);
        if (completion == EndpointCompletion::Stale) {
            ++endpointStaleCompletions_;
            portEXIT_CRITICAL(&schedulerMux_);
            return true;
        }
        item.epoch = state.outTransfer.epoch();
        item.bridgeSession = bridgeActiveSession();
        if (completion == EndpointCompletion::Delivered) {
            item.endpoint = endpoint;
            item.length = static_cast<uint16_t>(transferred);
            memcpy(item.data, state.outBuffer, transferred);
            queueOutput = true;
        } else {
            ++endpointSendErrors_;
        }
        state.outTransfer.finishCompletion(item.epoch);
        state.outNeedsRearm = true;
        portEXIT_CRITICAL(&schedulerMux_);
        if (queueOutput && (outputQueue_ == nullptr ||
            xQueueSend(outputQueue_, &item, 0) != pdTRUE)) {
            ++endpointOutputDrops_;
            ++endpointSendErrors_;
        }
        // UART forwarding is delegated; the dispatcher also provides bounded
        // retry if re-arming the OUT endpoint transiently fails.
        notifyInputTask();
        return true;
    }

    portENTER_CRITICAL(&schedulerMux_);
    const EndpointCompletion completion = state.inTransfer.beginCompletion(
        schedulerEpoch_, result == XFER_RESULT_SUCCESS, transferred, true);
    if (completion == EndpointCompletion::Stale) {
        ++endpointStaleCompletions_;
        portEXIT_CRITICAL(&schedulerMux_);
        return true;
    }
    const uint32_t completionEpoch = state.inTransfer.epoch();
    ++state.transfersCompleted;
    const bool fullSuccess = completion == EndpointCompletion::Delivered;
    if (result == XFER_RESULT_SUCCESS &&
        completion == EndpointCompletion::Retry) {
        ++endpointShortTransfers_;
    }
    if (state.activeInSynthetic || state.activeInMouseMerged) {
        portENTER_CRITICAL(&mouseMux_);
        const bool settled = mouseLedger_.settle(fullSuccess);
        if (!settled && mouseRateGate_.enabled()) {
            ++syntheticQueueErrors_;
        }
        portEXIT_CRITICAL(&mouseMux_);
    }
    if (!fullSuccess) {
        ++endpointSendErrors_;
        if (state.activeInPhysical) {
            memcpy(state.retryIn, state.activeOriginal,
                   state.activeOriginalLength);
            state.retryInLength = state.activeOriginalLength;
            state.retryInValid = true;
        }
    }
    if (fullSuccess && state.activeInSynthetic) {
        ++syntheticTransfersCompleted_;
        state.forceSyntheticNext = false;
    } else if (fullSuccess && state.activeNeedsStandalone) {
        state.forceSyntheticNext = true;
    }
    state.activeInPhysical = false;
    state.activeInSynthetic = false;
    state.activeInMouseMerged = false;
    state.activeNeedsStandalone = false;
    state.activeOriginalLength = 0;
    state.inTransfer.finishCompletion(completionEpoch);
    portEXIT_CRITICAL(&schedulerMux_);
    // Completion only wakes the non-blocking dispatcher.  It never waits for
    // another endpoint and never submits a transfer from this callback.
    notifyInputTask();
    return true;
}

void PassthroughDevice::discoverMouseEndpoint() {
    uint8_t currentClass = 0;
    uint8_t currentSubClass = 0;
    uint8_t currentProtocol = 0;
    uint8_t currentInterface = 0xFF;
    for (uint16_t offset = 0; offset < configurationDescriptorBytes_;) {
        const uint8_t length = configurationDescriptor_[offset];
        const uint8_t type = configurationDescriptor_[offset + 1];
        if (type == TUSB_DESC_INTERFACE && length >= 9) {
            currentInterface = configurationDescriptor_[offset + 2];
            currentClass = configurationDescriptor_[offset + 5];
            currentSubClass = configurationDescriptor_[offset + 6];
            currentProtocol = configurationDescriptor_[offset + 7];
        } else if (type == TUSB_DESC_ENDPOINT && length >= 7 &&
                   (configurationDescriptor_[offset + 2] & 0x80) != 0) {
            const uint8_t endpoint = configurationDescriptor_[offset + 2];
            if (currentClass == TUSB_CLASS_VENDOR_SPECIFIC &&
                currentSubClass == 0x5D && currentProtocol == 0x01) {
                xinputInEndpoint_ = endpoint;
            }
            if (mouseInEndpoint_ == 0 && currentClass == TUSB_CLASS_HID) {
                makcm::hid::MouseReportLayout candidate;
                const HidDescriptorState *descriptor =
                    findHidDescriptor(currentInterface);
                const bool parsed = descriptor != nullptr &&
                    makcm::hid::parseMouseReportDescriptor(
                        descriptor->data, descriptor->receivedLength,
                        candidate) &&
                    normalizeInjectableMouseLayout(candidate);
                const bool bootMouse =
                    currentSubClass == HID_SUBCLASS_BOOT &&
                    currentProtocol == HID_ITF_PROTOCOL_MOUSE;
                if (parsed) {
                    mouseInEndpoint_ = endpoint;
                    mouseLayout_ = candidate;
                } else if (bootMouse) {
                    candidate = makcm::hid::bootMouseLayout();
                    mouseInEndpoint_ = endpoint;
                    mouseLayout_ = candidate;
                }
            }
        }
        offset += length;
    }
}

bool PassthroughDevice::mouseMove(int64_t x, int64_t y,
                                  int64_t wheel, int64_t pan) {
    return addMouseDelta(x, y, wheel, pan);
}

void PassthroughDevice::configureMouseInjection(bool enabled,
                                                uint16_t rateHz) {
    portENTER_CRITICAL(&mouseMux_);
    mouseRateGate_.configure(enabled, rateHz);
    if (!enabled) mouseLedger_.reset();
    portEXIT_CRITICAL(&mouseMux_);
    notifyInputTask();
}

void PassthroughDevice::mouseButton(uint8_t button, bool pressed) {
    ++syntheticRequests_;
    if (mouseInEndpoint_ == 0 || !mouseLayout_.valid) {
        ++syntheticQueueErrors_;
        return;
    }
    portENTER_CRITICAL(&mouseMux_);
    if (!mouseRateGate_.enabled()) {
        ++syntheticSuppressed_;
        portEXIT_CRITICAL(&mouseMux_);
        return;
    }
    const bool changed = mouseLedger_.setButton(button, pressed);
    portEXIT_CRITICAL(&mouseMux_);
    if (changed) {
        ++syntheticQueued_;
        notifyInputTask();
    }
}

bool PassthroughDevice::addMouseDelta(int64_t x, int64_t y,
                                      int64_t wheel, int64_t pan) {
    ++syntheticRequests_;
    if (x == 0 && y == 0 && wheel == 0 && pan == 0) return true;
    if (mouseInEndpoint_ == 0 || !mouseLayout_.valid) {
        ++syntheticQueueErrors_;
        return false;
    }
    portENTER_CRITICAL(&mouseMux_);
    if (!mouseRateGate_.enabled()) {
        ++syntheticSuppressed_;
        portEXIT_CRITICAL(&mouseMux_);
        return true;
    }
    const bool accepted = mouseLedger_.add(
        x, y, wheel, pan, mouseLayout_.wheel.present,
        mouseLayout_.pan.present);
    portEXIT_CRITICAL(&mouseMux_);
    if (accepted) {
        ++syntheticQueued_;
    } else {
        ++syntheticQueueErrors_;
    }
    notifyInputTask();
    return accepted;
}

uint8_t PassthroughDevice::mouseButtons(const uint8_t *report,
                                        uint16_t length) const {
    return makcm::hid::readMouseButtons(mouseLayout_, report, length);
}
