#include "BridgeTransport.h"

#include "PassthroughDevice.h"
#include "InputMonitor.h"
#include "InitSettings.h"
#include "handleCommands.h"
#include "USBSetup.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "tusb.h"
#include <algorithm>
#include <atomic>

extern volatile bool deviceConnected;

namespace {

makcm::bridge::MixedStreamDecoder decoder;
SemaphoreHandle_t txMutex = nullptr;
portMUX_TYPE sequenceMux = portMUX_INITIALIZER_UNLOCKED;
uint16_t txSequence = 0;
volatile uint32_t rxFrames = 0;
volatile uint32_t rxErrors = 0;
bool profileCommitted = false;
uint32_t pendingSession = 0;
std::atomic<uint32_t> activeSession{0};
enum class ProxyState : uint8_t {
    WaitingProfile = 0,
    ReceivingProfile = 1,
    Enumerating = 2,
    WaitingMount = 3,
    Streaming = 4,
    Disconnecting = 5,
};
volatile ProxyState proxyState = ProxyState::WaitingProfile;
uint32_t lastProfileAcceptMillis = 0;

void tryAcceptProfile() {
    if (proxyState != ProxyState::WaitingMount || activeSession == 0 ||
        !Passthrough.inputEndpointReady()) {
        return;
    }
    const uint32_t now = millis();
    if (lastProfileAcceptMillis != 0 && now - lastProfileAcceptMillis < 250) {
        return;
    }
    if (sendBridgeFrame(makcm::bridge::MessageType::ProfileAccepted,
                        nullptr, 0)) {
        lastProfileAcceptMillis = now;
    }
}

void applyProfileMetadata(const makcm::bridge::DeviceProfile &profile) {
    descriptor_device.bLength = 18;
    descriptor_device.bDescriptorType = 1;
    descriptor_device.bcdUSB = profile.bcdUsb;
    descriptor_device.bDeviceClass = profile.deviceClass;
    descriptor_device.bDeviceSubClass = profile.deviceSubClass;
    descriptor_device.bDeviceProtocol = profile.deviceProtocol;
    descriptor_device.bMaxPacketSize0 = 64;
    descriptor_device.idVendor = profile.vid;
    descriptor_device.idProduct = profile.pid;
    descriptor_device.bcdDevice = profile.bcdDevice;
    descriptor_device.iManufacturer = profile.manufacturer[0] ? 1 : 0;
    descriptor_device.iProduct = profile.product[0] ? 2 : 0;
    descriptor_device.iSerialNumber = profile.serial[0] ? 3 : 0;
    descriptor_device.bNumConfigurations = 1;

    strlcpy(device_info.str_desc_manufacturer, profile.manufacturer,
            sizeof(device_info.str_desc_manufacturer));
    strlcpy(device_info.str_desc_product, profile.product,
            sizeof(device_info.str_desc_product));
    strlcpy(device_info.str_desc_serial_num, profile.serial,
            sizeof(device_info.str_desc_serial_num));
    device_info.vMaxPacketSize0 = 64;
    device_info.bConfigurationValue = 1;

    configuration_descriptor.bLength = 9;
    configuration_descriptor.bDescriptorType = 2;
    configuration_descriptor.bConfigurationValue = 1;
    configuration_descriptor.bmAttributes = profile.configAttributes;
    configuration_descriptor.bMaxPower = static_cast<uint8_t>(
        std::min<uint16_t>(250, profile.configPowerMa / 2));
}

void handleFrame(const makcm::bridge::Frame &frame) {
    switch (frame.type) {
    case makcm::bridge::MessageType::EndpointIn:
        if (proxyState != ProxyState::Streaming ||
            frame.session != activeSession || frame.length < 2 ||
            (frame.payload[0] & 0x80) == 0) {
            break;
        }
        if (descriptor_device.idVendor == 0x054C &&
            descriptor_device.idProduct == 0x0DF2) {
            updateDseTriggerMonitor(frame.payload + 1, frame.length - 1);
        } else if (Passthrough.isXinputEndpoint(frame.payload[0])) {
            updateXboxTriggerMonitor(frame.payload + 1, frame.length - 1);
        }
        if (Passthrough.isMouseInputEndpoint(frame.payload[0])) {
            const uint8_t buttons = Passthrough.mouseButtons(
                frame.payload + 1, frame.length - 1);
            updateMouseButtonMonitor(&buttons, 1, false);
        }
        Passthrough.queueEndpointInput(frame.payload[0], frame.payload + 1,
                                       frame.length - 1);
        break;
    case makcm::bridge::MessageType::VendorInput:
        if (proxyState != ProxyState::Streaming ||
            frame.session != activeSession) {
            break;
        }
        Passthrough.queueInput(makcm::bridge::MessageType::VendorInput,
                               frame.payload, frame.length);
        break;
    case makcm::bridge::MessageType::MouseInput:
        if (proxyState != ProxyState::Streaming ||
            frame.session != activeSession ||
            Passthrough.profile().kind != makcm::bridge::DeviceKind::XboxVendor) {
            break;
        }
        updateMouseButtonMonitor(
            frame.payload, frame.length,
            (Passthrough.profile().flags &
             makcm::bridge::kProfileFlagReportIds) != 0);
        Passthrough.queueInput(makcm::bridge::MessageType::MouseInput,
                               frame.payload, frame.length);
        break;
    case makcm::bridge::MessageType::HidFeatureResponse:
        if (!profileCommitted || frame.session != activeSession ||
            proxyState == ProxyState::Disconnecting) {
            break;
        }
        Passthrough.acceptFeatureResponse(frame.payload, frame.length);
        break;
    case makcm::bridge::MessageType::ControlResponse:
        if (profileCommitted && frame.session == activeSession &&
            proxyState != ProxyState::Disconnecting) {
            Passthrough.acceptControlResponse(frame.payload, frame.length);
        }
        break;
    case makcm::bridge::MessageType::ProfileBegin:
        if (frame.session == activeSession && profileCommitted) {
            break;
        }
        if (!profileCommitted &&
            proxyState == ProxyState::ReceivingProfile &&
            frame.session == pendingSession) {
            // The right repeats an unacknowledged profile. Preserve any
            // descriptor prefix already received so a dropped chunk can be
            // filled by the next transmission instead of restarting forever.
            break;
        }
        if (profileCommitted && frame.session != 0 &&
            frame.session != activeSession) {
            deviceConnected = false;
            Passthrough.disconnectUsb();
            vTaskDelay(pdMS_TO_TICKS(40));
            profileCommitted = false;
            activeSession = 0;
            pendingSession = 0;
            proxyState = ProxyState::WaitingProfile;
            resetInputMonitor();
            Passthrough.resetProfile();
        }
        if (!profileCommitted && frame.session != 0 &&
            frame.length == sizeof(makcm::bridge::DeviceProfile)) {
            makcm::bridge::DeviceProfile profile;
            memcpy(&profile, frame.payload, sizeof(profile));
            if (Passthrough.beginProfile(profile)) {
                pendingSession = frame.session;
                proxyState = ProxyState::ReceivingProfile;
                applyProfileMetadata(profile);
            }
        }
        break;
    case makcm::bridge::MessageType::ProfileDescriptorChunk:
        if (!profileCommitted && proxyState == ProxyState::ReceivingProfile &&
            frame.session == pendingSession &&
            frame.length >= 3) {
            const uint16_t offset = static_cast<uint16_t>(frame.payload[1]) |
                                    (static_cast<uint16_t>(frame.payload[2]) << 8);
            Passthrough.appendHidReportDescriptor(
                frame.payload[0], offset, frame.payload + 3,
                frame.length - 3);
        }
        break;
    case makcm::bridge::MessageType::ProfileDeviceDescriptorChunk:
        if (!profileCommitted && proxyState == ProxyState::ReceivingProfile &&
            frame.session == pendingSession && frame.length >= 2) {
            const uint16_t offset = static_cast<uint16_t>(frame.payload[0]) |
                                    (static_cast<uint16_t>(frame.payload[1]) << 8);
            Passthrough.appendDeviceDescriptor(offset, frame.payload + 2,
                                               frame.length - 2);
        }
        break;
    case makcm::bridge::MessageType::ProfileConfigurationChunk:
        if (!profileCommitted && proxyState == ProxyState::ReceivingProfile &&
            frame.session == pendingSession && frame.length >= 2) {
            const uint16_t offset = static_cast<uint16_t>(frame.payload[0]) |
                                    (static_cast<uint16_t>(frame.payload[1]) << 8);
            Passthrough.appendConfigurationDescriptor(
                offset, frame.payload + 2, frame.length - 2);
        }
        break;
    case makcm::bridge::MessageType::ProfileStringDescriptorChunk:
        if (!profileCommitted && proxyState == ProxyState::ReceivingProfile &&
            frame.session == pendingSession && frame.length >= 6) {
            const uint16_t langid = static_cast<uint16_t>(frame.payload[1]) |
                                    (static_cast<uint16_t>(frame.payload[2]) << 8);
            const uint16_t offset = static_cast<uint16_t>(frame.payload[3]) |
                                    (static_cast<uint16_t>(frame.payload[4]) << 8);
            Passthrough.appendStringDescriptor(
                frame.payload[0], langid, offset,
                frame.payload + 5, frame.length - 5);
        }
        break;
    case makcm::bridge::MessageType::ProfileReady:
        if (!profileCommitted && proxyState == ProxyState::ReceivingProfile &&
            frame.session == pendingSession &&
            Passthrough.finishProfile()) {
            profileCommitted = true;
            activeSession = pendingSession;
            deviceConnected = true;
            proxyState = ProxyState::Enumerating;
            InitUSB();
            proxyState = ProxyState::WaitingMount;
            serial0Locked = false;
        }
        if (profileCommitted && frame.session == activeSession) {
            tryAcceptProfile();
        }
        break;
    case makcm::bridge::MessageType::StreamStarted:
        if (profileCommitted && frame.session == activeSession &&
            proxyState == ProxyState::WaitingMount) {
            proxyState = ProxyState::Streaming;
        }
        break;
    case makcm::bridge::MessageType::DeviceGone:
        if (profileCommitted && frame.session == activeSession) {
            proxyState = ProxyState::Disconnecting;
            sendBridgeFrame(makcm::bridge::MessageType::DeviceGoneAccepted,
                            nullptr, 0);
            deviceConnected = false;
            Passthrough.disconnectUsb();
            vTaskDelay(pdMS_TO_TICKS(40));
            profileCommitted = false;
            activeSession = 0;
            pendingSession = 0;
            proxyState = ProxyState::WaitingProfile;
            resetInputMonitor();
            Passthrough.resetProfile();
        }
        break;
    default:
        break;
    }
}

} // namespace

void beginBridgeTransport() {
    if (txMutex == nullptr) {
        txMutex = xSemaphoreCreateMutex();
    }
}

void serviceBridgeTransport() {
    tryAcceptProfile();
}

bool consumeBridgeSerialByte(uint8_t byte) {
    makcm::bridge::Frame frame;
    uint8_t textByte = 0;
    const makcm::bridge::FeedResult result = decoder.feed(byte, frame, textByte);

    switch (result) {
    case makcm::bridge::FeedResult::TextByte:
        return false;
    case makcm::bridge::FeedResult::FrameReady:
        ++rxFrames;
        handleFrame(frame);
        return true;
    case makcm::bridge::FeedResult::FrameError:
        ++rxErrors;
        return true;
    case makcm::bridge::FeedResult::None:
    default:
        return true;
    }
}

bool sendBridgeFrame(makcm::bridge::MessageType type,
                     const uint8_t *payload,
                     uint16_t length) {
    return sendBridgeFrameForSession(type, payload, length,
                                     activeSession.load());
}

bool sendBridgeFrameForSession(makcm::bridge::MessageType type,
                               const uint8_t *payload, uint16_t length,
                               uint32_t session) {
    beginBridgeTransport();
    if (txMutex == nullptr) {
        return false;
    }

    uint16_t sequence;
    portENTER_CRITICAL(&sequenceMux);
    sequence = txSequence++;
    portEXIT_CRITICAL(&sequenceMux);

    uint8_t wire[makcm::bridge::kMaxWireFrameSize];
    const size_t wireLength = makcm::bridge::encodeFrame(
        type, sequence, payload, length, wire, sizeof(wire), session);
    if (wireLength == 0 || xSemaphoreTake(txMutex, pdMS_TO_TICKS(5)) != pdTRUE) {
        return false;
    }

    const size_t written = Serial1.write(wire, wireLength);
    xSemaphoreGive(txMutex);
    return written == wireLength;
}

bool sendBridgeText(const char *text, bool appendNewline) {
    if (text == nullptr) {
        return false;
    }
    beginBridgeTransport();
    if (txMutex == nullptr || xSemaphoreTake(txMutex, pdMS_TO_TICKS(5)) != pdTRUE) {
        return false;
    }

    const size_t length = strlen(text);
    const size_t written = Serial1.write(
        reinterpret_cast<const uint8_t *>(text), length);
    size_t newlineWritten = 1;
    if (appendNewline) {
        newlineWritten = Serial1.write(static_cast<uint8_t>('\n'));
    }
    xSemaphoreGive(txMutex);
    return written == length && newlineWritten == 1;
}

uint32_t bridgeRxFrameCount() {
    return rxFrames;
}

uint32_t bridgeRxErrorCount() {
    return rxErrors;
}

uint32_t bridgeActiveSession() {
    return activeSession.load();
}

uint8_t bridgeProxyState() {
    return static_cast<uint8_t>(proxyState);
}
