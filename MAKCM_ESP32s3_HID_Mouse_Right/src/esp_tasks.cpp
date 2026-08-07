#include "EspUsbHost.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

// Buffers defined here
RingBuf<char, 512> rxBuffer;
RingBuf<char, 512> txBuffer;
SemaphoreHandle_t ledSemaphore;
TaskHandle_t rxSerialTaskHandle;
#define USB_TASK_PRIORITY 1
#define CLIENT_TASK_PRIORITY 2

void EspUsbHost::begin(void)
{
    usbTransferSize = 0;
    usbInterfaceSize = 0;
    memset(usbTransfer, 0, sizeof(usbTransfer));
    memset(usbInterface, 0, sizeof(usbInterface));
    hostState = HostState::Empty;
    otaBootstrapPending = false;
    otaBootstrapTransaction = 0;
    otaBootstrapPeerGeneration = 0;
    otaBootstrapStartedMs = 0;
    otaHostReserved.store(false, std::memory_order_release);
    activeSessionId = 0;
    streamStartPending = false;
    downstreamSuspended = false;
    lastStreamStartedMillis = 0;
    streamStartRetries = 0;
    nextSessionId = esp_random();
    last_activity_time = millis();

    serial1Mutex = xSemaphoreCreateMutex();
    cleanupComplete = xSemaphoreCreateBinary();
    // OTA is initialized before the RX task, but remains quiescent until an
    // explicit OTA_ENTER maintenance command arrives on UART1.
    otaReceiver.begin(
        &Serial1, &serial1Mutex,
        [](void *context) -> bool {
            // Boot-local prerequisites only. Controller enumeration and USB
            // client readiness must not control bootloader rollback.
            auto *host = static_cast<EspUsbHost *>(context);
            return host != nullptr && host->serial1Mutex != nullptr &&
                   host->cleanupComplete != nullptr;
        },
        [](void *context) -> bool {
            // Pair/data-plane health remains strict and is reported separately.
            auto *host = static_cast<EspUsbHost *>(context);
            return host != nullptr && host->hostInstallStatus == ESP_OK &&
                   host->clientRegisterStatus == ESP_OK &&
                   host->hostState != EspUsbHost::HostState::Fault;
        },
        this);
    // An unreadable/corrupt OTA ledger is a data-plane safety fault, not just
    // an unavailable update service.  Keep USB Host enumeration reserved until
    // exact durable evidence is reconciled; otherwise NVS failure would reopen
    // the normal controller bridge.
    otaHostReserved.store(otaReceiver.isMaintenanceActive(),
                          std::memory_order_release);
    // Generic endpoint traffic gets one bounded FIFO per endpoint.  The
    // legacy queue remains only for old non-endpoint frames; it is never used
    // by EndpointInData, so one busy endpoint cannot evict another endpoint's
    // report.
    dseInputQueue = xQueueCreate(kInputQueueDepth, sizeof(InputQueueItem));
    bool endpointQueuesReady = true;
    for (uint8_t i = 0; i < kInputQueueCount; ++i) {
        endpointInputQueues[i] = xQueueCreate(kInputQueueDepth,
                                              sizeof(InputQueueItem));
        if (endpointInputQueues[i] == nullptr) endpointQueuesReady = false;
    }
    if (serial1Mutex == nullptr || cleanupComplete == nullptr ||
        dseInputQueue == nullptr || !endpointQueuesReady) {
        ESP_LOGE("EspUsbHost", "Failed to allocate DSE bridge synchronization objects.");
    }

    const usb_host_config_t host_config = {
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };

    hostInstallStatus = usb_host_install(&host_config);
    serial1Send("ESPLOG_HOST_INSTALL:%d\n", hostInstallStatus);

    ledSemaphore = xSemaphoreCreateBinary();

    if (xTaskCreate([](void *arg) { 
        static_cast<EspUsbHost *>(arg)->receiveSerial0(arg); 
    }, "RxTaskSerial0", 4096, this, 5, &rxSerialTaskHandle) != pdPASS) {
        ESP_LOGE("EspUsbHost", "Failed to create RxTaskSerial0.");
    }

    // OTA manifest verification (mbedtls ECDSA P-256) runs synchronously on
    // this task and needs 5-7 KB of stack on top of frame handling.  The
    // stack is static: a 16 KB heap allocation at boot must not be able to
    // fail or fragment the pool the USB host stack allocates from.
    static StackType_t rxSerial1Stack[16384];
    static StaticTask_t rxSerial1Tcb;
    if (xTaskCreateStatic([](void *arg) {
        static_cast<EspUsbHost *>(arg)->receiveSerial1(arg);
    }, "RxTaskSerial1", sizeof(rxSerial1Stack) / sizeof(StackType_t), this, 5,
                          rxSerial1Stack, &rxSerial1Tcb) == nullptr) {
        ESP_LOGE("EspUsbHost", "Failed to create RxTaskSerial1.");
    }

    if (xTaskCreate(EspUsbHost::dseUartTxTask, "DseUartTx", 3072, this, 6,
                    &dseUartTxTaskHandle) != pdPASS) {
        ESP_LOGE("EspUsbHost", "Failed to create DseUartTx task.");
    }

    if (xTaskCreate([](void *arg) { 
        static_cast<EspUsbHost *>(arg)->usbLibraryTask(arg); 
    }, "usbLibTask", 4096, this, USB_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE("EspUsbHost", "Failed to create usbLibTask.");
    }

    if (xTaskCreate([](void *arg) {
        static_cast<EspUsbHost *>(arg)->usbClientTask(arg); 
    }, "usbClientTask", 8192, this, CLIENT_TASK_PRIORITY,
       &usbClientTaskHandle) != pdPASS) {
        ESP_LOGE("EspUsbHost", "Failed to create usbClientTask.");
    }

    if (xTaskCreate([](void *arg) { 
        static_cast<EspUsbHost *>(arg)->monitorInactivity(arg); 
    }, "MonitorInactivityTask", 3200, this, 3, NULL) != pdPASS) {
        ESP_LOGE("EspUsbHost", "Failed to create MonitorInactivityTask.");
    }

    if (xTaskCreate(flashLEDToggleTask, "LED Flash Task", 1500, NULL, 1, NULL) != pdPASS) {
        ESP_LOGE("EspUsbHost", "Failed to create LED Flash Task.");
    }

    ESP_LOGI("EspUsbHost", "EspUsbHost::begin() completed. Tasks created.");
}



// combine both maybe mutex?

void handleSerialInput(HardwareSerial &serial, EspUsbHost *instance) {
    while (serial.available() > 0) {
        char byte = serial.read();

        if (byte == '\r') continue;

        if (!rxBuffer.isFull()) {
            rxBuffer.push(byte);
        } else {
            ESP_LOGW("EspUsbHost", "RX buffer overflow detected.");
            break;
        }

        if (byte == '\n') {
            char commandBuffer[620];
            int commandIndex = 0;

            while (!rxBuffer.isEmpty() && commandIndex < sizeof(commandBuffer) - 1) {
                rxBuffer.pop(commandBuffer[commandIndex++]);
                if (commandBuffer[commandIndex - 1] == '\n') break;
            }

            commandBuffer[commandIndex] = '\0';

            if (commandIndex > 0 && commandBuffer[commandIndex - 1] == '\n') {
                commandBuffer[commandIndex - 1] = '\0';
            }

            instance->handleIncomingCommands(commandBuffer, false);
        }
    }
}

void EspUsbHost::receiveSerial0(void *arg)
{
    EspUsbHost *instance = static_cast<EspUsbHost *>(arg);
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1));                                                               // Set 1ms, be lazy be happy
        handleSerialInput(Serial0, instance);
    }
}

// GO-1 diagnostic (remove after GO-1): UART1 RX hardware error counters.
// Discriminates the DATA#1 head-loss mechanism: FIFO_OVF means the RX ISR was
// stalled and bytes died in the 128-byte hardware FIFO, BUFFER_FULL means the
// 8248-byte driver ring overflowed (RX task stalled), zero counts alongside a
// corrupted frame means the bytes never left the left board intact.
// Non-static: ota_receiver.cpp reports the totals in its end-of-prepare crumb.
volatile uint32_t serial1RxFifoOvf = 0;
volatile uint32_t serial1RxBufferFull = 0;
volatile uint32_t serial1RxFrameErr = 0;

static void countSerial1RxError(hardwareSerial_error_t error)
{
    switch (error) {
    case UART_FIFO_OVF_ERROR:
        ++serial1RxFifoOvf;
        break;
    case UART_BUFFER_FULL_ERROR:
        ++serial1RxBufferFull;
        break;
    case UART_FRAME_ERROR:
        ++serial1RxFrameErr;
        break;
    default:
        break;
    }
}

void EspUsbHost::receiveSerial1(void *arg)
{
    EspUsbHost *instance = static_cast<EspUsbHost *>(arg);
    makcm::bridge::MixedStreamDecoder decoder;
    char commandBuffer[620] = {};
    size_t commandLength = 0;
    uint8_t otaMagicPending[sizeof(makcm::ota::kMagic)] = {};
    size_t otaMagicRun = 0;
    Serial1.onReceiveError(countSerial1RxError);

    while (true) {
        while (Serial1.available() > 0) {
            const uint8_t byte = static_cast<uint8_t>(Serial1.read());
            if (otaReceiver.isMaintenanceActive()) {
                if (otaReceiver.decoderBuffered() != 0) {
                    otaReceiver.feed(byte);
                    continue;
                }
                // Magic pre-hunt with text spill (the bridge
                // MixedStreamDecoder pattern): an MKOT frame is captured no
                // matter what half-assembled text occupies commandBuffer.
                // The previous "commandLength == 0" boundary rule let a
                // single glitched frame poison the line buffer with binary
                // residue, after which every retransmitted frame's 'M'
                // arrived mid-"line" and was eaten too -- one wire glitch
                // became a permanent DATA deadlock (GO-1 run tx=824070015).
                // Spilling a failed prefix keeps OTA_MAINT_BEGIN intact:
                // its mid-line 'M' is held, then re-emitted as text when
                // 'A' breaks the run.
                if (byte == makcm::ota::kMagic[otaMagicRun]) {
                    otaMagicPending[otaMagicRun++] = byte;
                    if (otaMagicRun == sizeof(makcm::ota::kMagic)) {
                        for (size_t i = 0; i < otaMagicRun; ++i) {
                            otaReceiver.feed(otaMagicPending[i]);
                        }
                        otaMagicRun = 0;
                    }
                    continue;
                }
                if (otaMagicRun != 0) {
                    // GO-1 diagnostic crumb (remove after GO-1): a 2+ byte
                    // magic prefix that failed mid-run is a strong hint of
                    // wire corruption inside a real frame header.
                    if (otaMagicRun >= 2) {
                        static uint8_t nearMissCrumbs = 0;
                        if (nearMissCrumbs < 3) {
                            ++nearMissCrumbs;
                            char crumb[32];
                            snprintf(crumb, sizeof(crumb), "OTA_MF S n=%u\n",
                                     static_cast<unsigned>(otaMagicRun));
                            otaReceiver.sendText(crumb);
                        }
                    }
                    for (size_t i = 0; i < otaMagicRun; ++i) {
                        if (commandLength < sizeof(commandBuffer) - 1) {
                            commandBuffer[commandLength++] =
                                static_cast<char>(otaMagicPending[i]);
                        } else {
                            commandLength = 0;
                        }
                    }
                    otaMagicRun = 0;
                    if (byte == makcm::ota::kMagic[0]) {
                        otaMagicPending[otaMagicRun++] = byte;
                        continue;
                    }
                }
                if (byte == '\r') continue;
                if (byte == '\n') {
                    commandBuffer[commandLength] = '\0';
                    if (commandLength != 0) {
                        // GO-1 diagnostic crumb (remove after GO-1): the only
                        // legitimate maintenance text from the left is
                        // OTA_MAINT_BEGIN; anything else is binary debris.
                        if (strncmp(commandBuffer, "OTA_", 4) != 0) {
                            static uint8_t textCrumbs = 0;
                            if (textCrumbs < 8) {
                                ++textCrumbs;
                                char crumb[32];
                                snprintf(crumb, sizeof(crumb),
                                         "OTA_MF X n=%u\n",
                                         static_cast<unsigned>(commandLength));
                                otaReceiver.sendText(crumb);
                            }
                        }
                        instance->handleIncomingCommands(commandBuffer, true);
                    }
                    commandLength = 0;
                } else if (commandLength < sizeof(commandBuffer) - 1) {
                    commandBuffer[commandLength++] = static_cast<char>(byte);
                } else {
                    commandLength = 0;
                    ESP_LOGW("EspUsbHost",
                             "Serial1 maintenance text overflow detected.");
                }
                continue;
            }
            otaMagicRun = 0;
            makcm::bridge::Frame frame;
            uint8_t textByte = 0;
            const makcm::bridge::FeedResult result = decoder.feed(byte, frame, textByte);

            if (result == makcm::bridge::FeedResult::FrameReady) {
                instance->handleBridgeFrame(frame);
                continue;
            }
            if (result == makcm::bridge::FeedResult::FrameError) {
                ++instance->dseSinkState.bridgeRxErrors;
                continue;
            }
            if (result != makcm::bridge::FeedResult::TextByte || textByte == '\r') {
                continue;
            }

            if (textByte == '\n') {
                commandBuffer[commandLength] = '\0';
                if (commandLength != 0) {
                    instance->handleIncomingCommands(commandBuffer, true);
                }
                commandLength = 0;
            } else if (commandLength < sizeof(commandBuffer) - 1) {
                commandBuffer[commandLength++] = static_cast<char>(textByte);
            } else {
                commandLength = 0;
                ESP_LOGW("EspUsbHost", "Serial1 text command overflow detected.");
            }
        }
        // Keep servicing the deferred OTA bootstrap after every receive
        // slice.  This task must remain alive while the USB client task waits
        // for DeviceGoneAccepted; handling the command synchronously here
        // would deadlock that acknowledgement path.
        instance->serviceOtaBootstrap();
        otaReceiver.tick();
        // GO-1 diagnostic crumb (remove after GO-1): report UART1 RX hardware
        // error counters the moment they move during maintenance.
        if (otaReceiver.isMaintenanceActive()) {
            static uint32_t reportedOvf = 0;
            static uint32_t reportedFull = 0;
            static uint32_t reportedFrame = 0;
            static uint8_t errorCrumbs = 0;
            if (errorCrumbs < 4 && (serial1RxFifoOvf != reportedOvf ||
                                    serial1RxBufferFull != reportedFull ||
                                    serial1RxFrameErr != reportedFrame)) {
                ++errorCrumbs;
                reportedOvf = serial1RxFifoOvf;
                reportedFull = serial1RxBufferFull;
                reportedFrame = serial1RxFrameErr;
                char crumb[48];
                snprintf(crumb, sizeof(crumb),
                         "OTA_MF V ovf=%u full=%u frm=%u\n",
                         static_cast<unsigned>(reportedOvf),
                         static_cast<unsigned>(reportedFull),
                         static_cast<unsigned>(reportedFrame));
                otaReceiver.sendText(crumb);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

bool EspUsbHost::serial1Send(const char *format, ...)
{
    char logMsg[620];

    va_list args;
    va_start(args, format);
    vsnprintf(logMsg, sizeof(logMsg), format, args);
    va_end(args);

    return serial1Write(reinterpret_cast<const uint8_t *>(logMsg), strlen(logMsg));
}

bool EspUsbHost::serial1Write(const uint8_t *data, size_t length)
{
    // Once maintenance mode owns UART1, dropping normal bridge/log writes is
    // required to keep binary OTA frames unambiguous and to fail closed for
    // the USB forwarding path.
    if (otaReceiver.isMaintenanceActive()) return false;
    if (data == nullptr || serial1Mutex == nullptr ||
        xSemaphoreTake(serial1Mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }

    // Maintenance may have started while this task waited for the wire lock.
    // Recheck under the same mutex used by OTA responses so ordinary bridge
    // bytes cannot be emitted after binary ownership is established.
    if (otaReceiver.isMaintenanceActive()) {
        xSemaphoreGive(serial1Mutex);
        return false;
    }

    const size_t written = Serial1.write(data, length);
    xSemaphoreGive(serial1Mutex);
    return written == length;
}

void EspUsbHost::sendStreamStarted()
{
    if (activeSessionId == 0 || !deviceConnected ||
        hostState != HostState::Active || downstreamSuspended) {
        return;
    }
    // Keep the gate closed until the left device task confirms it consumed
    // this frame. A lost StreamStarted is retried by monitorInactivity().
    deviceMouseReady = false;
    streamStartPending = true;
    lastStreamStartedMillis = millis();
    ++streamStartRetries;
    sendBridgeFrame(makcm::bridge::MessageType::StreamStarted, 0, nullptr,
                    0, activeSessionId);
}

bool EspUsbHost::sendBridgeFrame(makcm::bridge::MessageType type,
                                 uint16_t sequence,
                                 const uint8_t *payload,
                                 uint16_t length,
                                 uint32_t session)
{
    uint8_t wire[makcm::bridge::kMaxWireFrameSize];
    const size_t wireLength = makcm::bridge::encodeFrame(
        type, sequence, payload, length, wire, sizeof(wire),
        session == UINT32_MAX ? activeSessionId : session);
    return wireLength != 0 && serial1Write(wire, wireLength);
}

void EspUsbHost::handleBridgeFrame(const makcm::bridge::Frame &frame)
{
    const bool activeFrame = frame.session != 0 &&
                             frame.session == activeSessionId;
    switch (frame.type) {
    case makcm::bridge::MessageType::EndpointOutData:
    case makcm::bridge::MessageType::EndpointOut: {
        if (!activeFrame || !deviceConnected ||
            hostState != HostState::Active) {
            break;
        }
        dseSinkState.lastOutputLength = 0;
        if (frame.length != 0) {
            memcpy(dseSinkState.lastOutput, frame.payload, frame.length);
        }
        dseSinkState.lastOutputSequence = frame.sequence;
        dseSinkState.lastOutputLength = frame.length;
        ++dseSinkState.outputCount;
        const bool generic = frame.type ==
            makcm::bridge::MessageType::EndpointOutData;
        const uint16_t metadata = generic ? 2 : 1;
        if (frame.length > metadata) {
            submitEndpointOutput(frame.payload[0], frame.payload + metadata,
                                 static_cast<uint16_t>(frame.length - metadata));
        }
        break;
    }

    case makcm::bridge::MessageType::ControlSetup:
    case makcm::bridge::MessageType::ControlRequest: {
        if (!activeFrame || !deviceConnected ||
            hostState != HostState::Active ||
            frame.length < sizeof(makcm::bridge::ControlRequestHeader)) {
            break;
        }
        makcm::bridge::ControlRequestHeader request;
        memcpy(&request, frame.payload, sizeof(request));
        if (sizeof(request) + request.dataLength != frame.length) {
            break;
        }
        submitProxyControl(request, frame.payload + sizeof(request),
                           request.dataLength, frame.session);
        break;
    }

    case makcm::bridge::MessageType::ControlOutData: {
        if (!activeFrame || !deviceConnected ||
            hostState != HostState::Active ||
            frame.length < sizeof(makcm::bridge::ControlOutDataHeader)) {
            break;
        }
        makcm::bridge::ControlOutDataHeader dataHeader;
        memcpy(&dataHeader, frame.payload, sizeof(dataHeader));
        const uint16_t dataLength = static_cast<uint16_t>(
            frame.length - sizeof(dataHeader));
        const uint32_t end = static_cast<uint32_t>(dataHeader.offset) +
                             dataLength;
        if ((dataHeader.request.bmRequestType & 0x80) != 0 ||
            dataHeader.totalLength == 0 ||
            dataHeader.request.wLength != dataHeader.totalLength ||
            dataHeader.request.dataLength != dataHeader.totalLength ||
            end > dataHeader.totalLength ||
            dataHeader.offset != 0 || dataLength != dataHeader.totalLength) {
            break;
        }
        submitProxyControl(dataHeader.request,
                           frame.payload + sizeof(dataHeader), dataLength,
                           frame.session);
        break;
    }

    case makcm::bridge::MessageType::BusReset:
        if (activeFrame) {
            // The downstream host stack owns the physical reset.  This event
            // is an ordering/diagnostic marker so the two endpoint epochs can
            // be correlated without pretending to forward USB tokens.
            serial1Send("ESPLOG_BUS_RESET session=%08lx\n",
                        static_cast<unsigned long>(frame.session));
        }
        break;

    case makcm::bridge::MessageType::SetConfiguration:
        if (activeFrame && frame.length == sizeof(
                makcm::bridge::SetConfigurationHeader)) {
            makcm::bridge::SetConfigurationHeader state;
            memcpy(&state, frame.payload, sizeof(state));
            serial1Send("ESPLOG_SET_CONFIGURATION value=%u\n",
                        static_cast<unsigned>(state.configuration));
        }
        break;

    case makcm::bridge::MessageType::SetInterface:
        if (activeFrame && frame.length == sizeof(
                makcm::bridge::SetInterfaceHeader)) {
            makcm::bridge::SetInterfaceHeader state;
            memcpy(&state, frame.payload, sizeof(state));
            serial1Send("ESPLOG_SET_INTERFACE if=%u alt=%u\n",
                        static_cast<unsigned>(state.interfaceNumber),
                        static_cast<unsigned>(state.alternateSetting));
        }
        break;

    case makcm::bridge::MessageType::SuspendResume:
        if (activeFrame && frame.length == sizeof(
                makcm::bridge::SuspendResumeHeader)) {
            makcm::bridge::SuspendResumeHeader state;
            memcpy(&state, frame.payload, sizeof(state));
            downstreamSuspended = state.suspended != 0;
            deviceMouseReady = false;
            streamStartPending = false;
            resetInputQueues();
            serial1Send("ESPLOG_SUSPEND suspended=%u wake=%u\n",
                        static_cast<unsigned>(state.suspended),
                        static_cast<unsigned>(state.remoteWakeup));
            if (!downstreamSuspended) sendStreamStarted();
        }
        break;

    case makcm::bridge::MessageType::StallHalt:
        if (activeFrame && frame.length == sizeof(
                makcm::bridge::EndpointStatusHeader)) {
            makcm::bridge::EndpointStatusHeader state;
            memcpy(&state, frame.payload, sizeof(state));
            serial1Send("ESPLOG_ENDPOINT_STATUS ep=0x%02x flags=0x%02x\n",
                        static_cast<unsigned>(state.endpoint),
                        static_cast<unsigned>(state.flags));
        }
        break;

    case makcm::bridge::MessageType::HidFeatureSet:
        if (!activeFrame || !deviceConnected ||
            hostState != HostState::Active) {
            break;
        }
        dseSinkState.lastFeatureSetLength = 0;
        if (frame.length != 0) {
            memcpy(dseSinkState.lastFeatureSet, frame.payload, frame.length);
        }
        dseSinkState.lastFeatureSetSequence = frame.sequence;
        dseSinkState.lastFeatureSetLength = frame.length;
        ++dseSinkState.featureSetCount;
        submitFeatureSet(frame.payload, frame.length);
        break;

    case makcm::bridge::MessageType::HidFeatureGet:
        if (!activeFrame || !deviceConnected ||
            hostState != HostState::Active) {
            break;
        }
        if (frame.length == 3) {
            submitFeatureGet(frame.payload[0],
                static_cast<uint16_t>(frame.payload[1]) |
                (static_cast<uint16_t>(frame.payload[2]) << 8));
        }
        break;

    case makcm::bridge::MessageType::VendorOutput:
        if (activeFrame && deviceConnected &&
            hostState == HostState::Active) {
            submitOutputReport(frame.payload, frame.length);
        }
        break;

    case makcm::bridge::MessageType::ProfileAccepted:
        // The left USB device stack is configured and ready to consume input.
        // Keep this acknowledgement binary so it shares the same framing,
        // CRC and retry behavior as the profile that it acknowledges.
        if (activeFrame && deviceConnected && hostState == HostState::Active) {
            sendStreamStarted();
        }
        break;

    case makcm::bridge::MessageType::StreamReady:
        if (activeFrame && deviceConnected &&
            hostState == HostState::Active && streamStartPending &&
            !downstreamSuspended) {
            streamStartPending = false;
            deviceMouseReady = true;
            serial1Send("ESPLOG_STREAM_READY retries=%lu\n",
                        static_cast<unsigned long>(streamStartRetries));
        }
        break;

    case makcm::bridge::MessageType::CapabilityResult:
        // A left-side validation failure is reported with the same structured
        // capability record that gated D+. Keep it visible on the CH343
        // diagnostics path instead of waiting for the next profile retry to
        // make the reason disappear.
        if (activeFrame && frame.length == sizeof(
                makcm::bridge::CapabilityResult)) {
            makcm::bridge::CapabilityResult result;
            memcpy(&result, frame.payload, sizeof(result));
            if (result.capability ==
                    makcm::bridge::ProxyCapability::Unsupported) {
                serial1Send("ESPLOG_PROFILE_REJECT reason=%u endpoints=%u "
                            "in=%u out=%u\n",
                            static_cast<unsigned>(result.reason),
                            static_cast<unsigned>(result.endpointCount),
                            static_cast<unsigned>(result.inCount),
                            static_cast<unsigned>(result.outCount));
            }
        }
        break;

    case makcm::bridge::MessageType::DeviceGoneAccepted:
        if (frame.session != 0 && frame.session == closingSessionId) {
            deviceGoneAccepted = true;
        }
        break;

    default:
        break;
    }
}

void EspUsbHost::queueInputReport(makcm::bridge::MessageType type,
                                  const uint8_t *data, uint16_t length)
{
    if (downstreamSuspended || !deviceMouseReady || data == nullptr ||
        length == 0 || length > sizeof(InputQueueItem::data)) {
        return;
    }

    InputQueueItem item;
    item.type = type;
    item.session = activeSessionId;
    item.length = length;
    memcpy(item.data, data, length);

    QueueHandle_t queue = nullptr;
    if (type == makcm::bridge::MessageType::EndpointInData ||
        type == makcm::bridge::MessageType::EndpointIn) {
        const uint8_t endpoint = data[0];
        const uint8_t number = endpoint & 0x0F;
        if ((endpoint & 0x80) != 0 && number < kInputQueueCount)
            queue = endpointInputQueues[number];
    }
    if (queue == nullptr) queue = dseInputQueue;
    if (queue == nullptr) return;

    // Ordered endpoint FIFOs use drop-newest backpressure.  Never remove an
    // older report to make space: doing so silently destroys relative-motion
    // conservation and makes a busy endpoint affect another endpoint.
    if (uxQueueSpacesAvailable(queue) == 0) {
        ++dseSinkState.inputQueueOverwrites;
        return;
    }
    if (xQueueSend(queue, &item, 0) == pdTRUE) {
        ++inputForwardCount;
    }
}

void EspUsbHost::resetInputQueues()
{
    if (dseInputQueue != nullptr) xQueueReset(dseInputQueue);
    for (uint8_t i = 0; i < kInputQueueCount; ++i) {
        if (endpointInputQueues[i] != nullptr)
            xQueueReset(endpointInputQueues[i]);
    }
    inputQueueRoundRobin = 0;
}

void EspUsbHost::dseUartTxTask(void *parameter)
{
    EspUsbHost *instance = static_cast<EspUsbHost *>(parameter);
    InputQueueItem item;
    uint16_t sequence = 0;

    while (true) {
        // Do not dequeue reports while the upstream consumer is closed.  The
        // suspend handler resets the bounded queues; this guard prevents a
        // race from turning the TX task into a busy loop that silently drains
        // reports while StreamReady is still pending.
        if (instance->downstreamSuspended || !deviceMouseReady) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        bool received = false;
        // Round-robin over endpoint numbers.  This is fairness, not a
        // class/endpoint priority: every queue receives the same service
        // opportunity and preserves its own FIFO order.
        for (uint8_t step = 0; step < EspUsbHost::kInputQueueCount; ++step) {
            const uint8_t index = static_cast<uint8_t>(
                (instance->inputQueueRoundRobin + step) %
                EspUsbHost::kInputQueueCount);
            QueueHandle_t queue = instance->endpointInputQueues[index];
            if (queue != nullptr && xQueueReceive(queue, &item, 0) == pdTRUE) {
                instance->inputQueueRoundRobin = static_cast<uint8_t>(index + 1);
                received = true;
                break;
            }
        }
        if (!received && instance->dseInputQueue != nullptr &&
            xQueueReceive(instance->dseInputQueue, &item, 0) == pdTRUE) {
            received = true;
        }
        if (received && !instance->downstreamSuspended && deviceMouseReady &&
            !instance->sendBridgeFrame(item.type, sequence++, item.data,
                                       item.length, item.session)) {
            ++instance->dseSinkState.inputTxErrors;
        } else if (!received) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
}

void EspUsbHost::monitorInactivity(void *arg)
{
    EspUsbHost *usbHost = static_cast<EspUsbHost *>(arg);
    uint32_t lastHelloMillis = 0;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(100));

        if (usbHost->hostState == HostState::Active &&
            usbHost->deviceConnected && usbHost->streamStartPending &&
            !deviceMouseReady && !usbHost->downstreamSuspended &&
            millis() - usbHost->lastStreamStartedMillis >= 250) {
            usbHost->sendStreamStarted();
        }

        // Device discovery and left-MCU boot are independent. Re-advertise the
        // complete binary profile until the left confirms ProfileAccepted.
        if (usbHost->hostState == HostState::Active &&
            usbHost->deviceConnected && !deviceMouseReady &&
            !usbHost->downstreamSuspended &&
            usbHost->reportDescriptorReady &&
            usbHost->profileStableSinceMs != 0 &&
            millis() - usbHost->profileStableSinceMs >= 600 &&
            millis() - lastHelloMillis >= 2000) {
            usbHost->sendPassthroughProfile();
            lastHelloMillis = millis();
        }

        // A transparent host must keep the downstream interrupt transfer
        // armed. Wireless mice commonly send nothing while stationary; using
        // that silence as an inactivity timeout prevents their next movement
        // from ever reaching us. Upstream USB suspend/resume must be handled
        // explicitly, not inferred from missing input reports.
    }
}

void EspUsbHost::usbLibraryTask(void *arg)
{
    EspUsbHost *instance = static_cast<EspUsbHost *>(arg);

    // Register the client before waiting for the first library event. If a
    // controller is already attached while the bridge boots, registering only
    // after usb_host_lib_handle_events() wakes can miss its NEW_DEV event.
    while (instance->clientHandle == NULL || !instance->isClientRegistering) {
        const usb_host_client_config_t client_config = {
            .max_num_event_msg = 10,
            .async = {
                .client_event_callback = instance->_clientEventCallback,
                .callback_arg = instance,
            }
        };

        const esp_err_t register_err =
            usb_host_client_register(&client_config, &instance->clientHandle);
        instance->clientRegisterStatus = register_err;
        if (register_err == ESP_OK) {
            instance->isClientRegistering = true;
            instance->serial1Send("ESPLOG_CLIENT_REGISTER:0\n");
            ESP_LOGI("EspUsbHost", "Client registered successfully.");
            break;
        }

        ESP_LOGW("EspUsbHost", "Failed to register client, retrying: %x", register_err);
        instance->serial1Send("ESPLOG_CLIENT_REGISTER:%d\n", register_err);
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    while (true) {
        uint32_t event_flags;
        esp_err_t err = usb_host_lib_handle_events(portMAX_DELAY, &event_flags);

        if (err != ESP_OK) {
            ESP_LOGE("EspUsbHost", "usb_host_lib_handle_events() failed with error: %x", err);
        }
    }
}


void EspUsbHost::usbClientTask(void *arg)
{
    EspUsbHost *instance = static_cast<EspUsbHost *>(arg);

    while (true) {
        if (!instance->isClientRegistering) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        // Keep the client task as the sole owner of client callbacks and
        // device/interface lifecycle APIs.  A finite timeout also lets us
        // service a fast re-plug queued while the previous device is being
        // cleaned up.
        const esp_err_t err = usb_host_client_handle_events(
            instance->clientHandle, pdMS_TO_TICKS(10));
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            ESP_LOGE("EspUsbHost", "usb_host_client_handle_events() failed: %x", err);
        }

        if (instance->cleanupRequested &&
            instance->hostState == EspUsbHost::HostState::Cleaning) {
            instance->cleanupRequested = false;
            instance->cleanupDevice();
        }

        // NEW_DEV received while Cleaning is deliberately handled here, not
        // by CleanupTask.  Calling _clientEventCallback from another task
        // violates the ESP-IDF USB Host client-context contract.
        if (!instance->otaHostReserved.load(std::memory_order_acquire) &&
            instance->hostState == EspUsbHost::HostState::Empty &&
            instance->pendingDeviceAddress != 0) {
            const uint8_t address = instance->pendingDeviceAddress;
            instance->pendingDeviceAddress = 0;
            usb_host_client_event_msg_t pendingEvent = {};
            pendingEvent.event = USB_HOST_CLIENT_EVENT_NEW_DEV;
            pendingEvent.new_dev.address = address;
            EspUsbHost::_clientEventCallback(&pendingEvent, instance);
        }
    }
}

void flashLEDToggleTask(void *parameter)
{
    pinMode(9, OUTPUT);

    while (true) {
        if (xSemaphoreTake(ledSemaphore, portMAX_DELAY) == pdTRUE) {
            digitalWrite(9, HIGH);
            vTaskDelay(pdMS_TO_TICKS(25));
            digitalWrite(9, LOW);
        }
    }
}

void flashLED()
{
    if (ledSemaphore != NULL) {
        xSemaphoreGive(ledSemaphore);
    }
}
