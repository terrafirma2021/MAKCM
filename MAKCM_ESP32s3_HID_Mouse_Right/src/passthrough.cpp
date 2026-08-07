#include "EspUsbHost.h"
#include "ProxyCapabilityPlanner.h"

#include <algorithm>
#include <cstring>
#include <new>

namespace {

struct FeatureGetContext {
    EspUsbHost *host = nullptr;
    uint32_t session = 0;
};

struct ProxyControlContext {
    EspUsbHost *host = nullptr;
    uint16_t requestId = 0;
    uint32_t session = 0;
    bool directionIn = false;
    uint16_t expectedLength = 0;
    uint8_t ep0PacketSize = 64;
};

} // namespace

void EspUsbHost::sendRuntimeStatus()
{
    uint16_t microsoftOsStringLength = 0;
    for (uint8_t i = 0; i < proxyStringDescriptorCount; ++i) {
        if (proxyStringDescriptors[i].index == 0xEE) {
            microsoftOsStringLength = proxyStringDescriptors[i].receivedLength;
            break;
        }
    }
    serial1Send(
        "ESPLOG_STATUS host=%d client=%d registered=%u connected=%u events=%lu "
        "vid=%04x pid=%04x interfaces=%d if=%u kind=%u "
        "in=0x%02x out=0x%02x auxif=%u auxin=0x%02x auxout=0x%02x "
        "report=%u profile=%u ack=%u stream=%u/%lu suspend=%u "
        "state=%u reset=%u clienthwm=%u session=%08lx closing=%08lx "
        "goneack=%u clean=%lu cleanerr=%lu input=%lu forward=%lu "
        "txerr=%lu overwrite=%lu rxerr=%lu str=%u msos=%u "
        "cfg=%u/%u if=%u crc=%04x\n",
        hostInstallStatus, clientRegisterStatus, isClientRegistering ? 1 : 0,
        deviceConnected ? 1 : 0, static_cast<unsigned long>(newDeviceEventCount),
        descriptor_device.idVendor, descriptor_device.idProduct, interfaceCounter,
        selectedInterface, static_cast<unsigned>(passthroughProfile.kind),
        selectedInEndpoint, selectedOutEndpoint,
        auxiliaryMouseInterface, auxiliaryMouseInEndpoint,
        auxiliaryMouseOutEndpoint, reportDescriptorLength,
        reportDescriptorReady ? 1 : 0, deviceMouseReady ? 1 : 0,
        streamStartPending ? 1 : 0,
        static_cast<unsigned long>(streamStartRetries),
        downstreamSuspended ? 1 : 0,
        static_cast<unsigned>(hostState.load(std::memory_order_acquire)),
        static_cast<unsigned>(esp_reset_reason()),
        static_cast<unsigned>(usbClientTaskHandle != nullptr
            ? uxTaskGetStackHighWaterMark(usbClientTaskHandle) : 0),
        static_cast<unsigned long>(activeSessionId),
        static_cast<unsigned long>(closingSessionId),
        deviceGoneAccepted ? 1 : 0,
        static_cast<unsigned long>(cleanupCount),
        static_cast<unsigned long>(cleanupErrorCount),
        static_cast<unsigned long>(inputReportCount),
        static_cast<unsigned long>(inputForwardCount),
        static_cast<unsigned long>(dseSinkState.inputTxErrors),
        static_cast<unsigned long>(dseSinkState.inputQueueOverwrites),
        static_cast<unsigned long>(dseSinkState.bridgeRxErrors),
        static_cast<unsigned>(proxyStringDescriptorCount),
        static_cast<unsigned>(microsoftOsStringLength),
        static_cast<unsigned>(rawConfigurationDescriptorLength),
        static_cast<unsigned>(rawConfigurationDeclaredLength),
        static_cast<unsigned>(rawConfigurationInterfaceCount),
        static_cast<unsigned>(rawConfigurationCrc16));

    serial1Send("ESPLOG_CAPABILITY raw=%u reason=%u speed=%u blobs=%u bos=%u qualifier=%u\n",
                rawTopologySupported ? 1u : 0u,
                static_cast<unsigned>(rawTopologyRejectReason),
                static_cast<unsigned>(passthroughProfile.usbSpeed),
                static_cast<unsigned>(passthroughProfile.blobCount),
                static_cast<unsigned>(rawBosDescriptorLength),
                static_cast<unsigned>(rawQualifierDescriptorLength));

    // Keep the compact status line useful for normal health checks, but also
    // expose the raw topology needed to identify controllers which enumerate
    // in more than one USB mode (for example XInput vs. a vendor/HID mode).
    // One line per descriptor stays comfortably below the bridge text limit.
    for (int i = 0; i < interfaceCounter; ++i) {
        const auto &intf = interface_descriptors[i];
        serial1Send(
            "ESPLOG_STATUS_IF idx=%d num=%u alt=%u eps=%u "
            "class=0x%02x subclass=0x%02x protocol=0x%02x str=%u\n",
            i, intf.bInterfaceNumber, intf.bAlternateSetting,
            intf.bNumEndpoints, intf.bInterfaceClass,
            intf.bInterfaceSubClass, intf.bInterfaceProtocol,
            intf.iInterface);
    }
    for (int i = 0; i < endpointCounter; ++i) {
        const auto &ep = endpoint_descriptors[i];
        serial1Send(
            "ESPLOG_STATUS_EP idx=%d addr=0x%02x attr=0x%02x "
            "max=%u interval=%u\n",
            i, ep.bEndpointAddress, ep.bmAttributes,
            ep.wMaxPacketSize, ep.bInterval);
    }
    for (uint8_t i = 0; i < proxyStringDescriptorCount; ++i) {
        const auto &str = proxyStringDescriptors[i];
        if (str.index != 0xEE || str.receivedLength == 0) continue;
        char line[128]{};
        int used = snprintf(line, sizeof(line),
                            "ESPLOG_STATUS_MSOS len=%u hex=",
                            static_cast<unsigned>(str.receivedLength));
        for (uint16_t j = 0; j < str.receivedLength &&
                            used > 0 && used + 3 < sizeof(line); ++j) {
            used += snprintf(line + used, sizeof(line) - used, "%02x",
                             str.data[j]);
        }
        if (used > 0 && used + 2 < sizeof(line)) {
            line[used++] = '\r';
            line[used++] = '\n';
            line[used] = '\0';
            serial1Send("%s", line);
        }
    }
    for (uint8_t i = 0; i < proxyControlCacheCount; ++i) {
        const auto &entry = proxyControlCache[i];
        char line[520]{};
        int used = snprintf(
            line, sizeof(line),
            "ESPLOG_STATUS_CACHE idx=%u bm=%02x req=%02x value=%04x "
            "index=%04x len=%u hex=",
            static_cast<unsigned>(i),
            static_cast<unsigned>(entry.bmRequestType),
            static_cast<unsigned>(entry.bRequest),
            static_cast<unsigned>(entry.wValue),
            static_cast<unsigned>(entry.wIndex),
            static_cast<unsigned>(entry.receivedLength));
        for (uint16_t j = 0; j < entry.receivedLength && used > 0 &&
                            used + 3 < sizeof(line); ++j) {
            used += snprintf(line + used, sizeof(line) - used, "%02x",
                             entry.data[j]);
        }
        if (used > 0 && used + 2 < sizeof(line)) {
            line[used++] = '\r';
            line[used++] = '\n';
            line[used] = '\0';
            serial1Send("%s", line);
        }
    }
}

void EspUsbHost::sendPassthroughProfile()
{
    for (int attempt = 0; attempt < 60 && !reportDescriptorReady; ++attempt) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (passthroughProfile.kind != makcm::bridge::DeviceKind::RawUsb ||
        rawDeviceDescriptorLength != sizeof(rawDeviceDescriptor) ||
        rawConfigurationDescriptorLength < 9) {
        ESP_LOGE("EspUsbHost", "Passthrough profile descriptors are not ready");
        serial1Send("USB_PROFILE_NOT_READY descriptors\n");
        return;
    }

    passthroughProfile.vid = descriptor_device.idVendor;
    passthroughProfile.pid = descriptor_device.idProduct;
    passthroughProfile.bcdUsb = descriptor_device.bcdUSB;
    passthroughProfile.bcdDevice = descriptor_device.bcdDevice;
    passthroughProfile.deviceClass = descriptor_device.bDeviceClass;
    passthroughProfile.deviceSubClass = descriptor_device.bDeviceSubClass;
    passthroughProfile.deviceProtocol = descriptor_device.bDeviceProtocol;
    passthroughProfile.configAttributes = descriptor_configuration.bmAttributes;
    // configurationDescriptor stores the raw USB bMaxPower value in 2 mA
    // units. descriptor_configuration is a legacy structure whose uint8_t
    // field cannot hold a value such as 500 mA without wrapping.
    passthroughProfile.configPowerMa =
        static_cast<uint16_t>(configurationDescriptor.bMaxPower) * 2;
    passthroughProfile.deviceDescriptorLength = rawDeviceDescriptorLength;
    passthroughProfile.configurationDescriptorLength =
        rawConfigurationDescriptorLength;
    passthroughProfile.endpointCount = proxyEndpointCount;
    // Arduino's bundled USB host headers expose LOW/FULL in this target but
    // do not export the HIGH enum used by newer IDF headers.  The enum values
    // are stable (0=low, 1=full, 2=high), so keep the mapping numeric and
    // preserve a future/unknown value as Unknown instead of guessing Full.
    switch (device_info.speed) {
    case 0:
        passthroughProfile.usbSpeed = makcm::bridge::UsbSpeed::Low;
        break;
    case 1:
        passthroughProfile.usbSpeed = makcm::bridge::UsbSpeed::Full;
        break;
    case 2:
        passthroughProfile.usbSpeed = makcm::bridge::UsbSpeed::High;
        break;
    default:
        passthroughProfile.usbSpeed = makcm::bridge::UsbSpeed::Unknown;
        break;
    }
    passthroughProfile.configurationCount =
        descriptor_device.bNumConfigurations;
    passthroughProfile.bosDescriptorLength = rawBosDescriptorLength;
    passthroughProfile.qualifierDescriptorLength =
        rawQualifierDescriptorLength;
    uint8_t receivedStringBlobCount = 0;
    for (uint8_t i = 0; i < proxyStringDescriptorCount; ++i) {
        if (proxyStringDescriptors[i].receivedLength != 0)
            ++receivedStringBlobCount;
    }
    passthroughProfile.blobCount = static_cast<uint8_t>(
        2 + proxyHidDescriptorCount + receivedStringBlobCount +
        (rawBosDescriptorLength != 0 ? 1 : 0) +
        (rawQualifierDescriptorLength != 0 ? 1 : 0));
    uint32_t aggregateCrc = 0;
    auto addAggregate = [&aggregateCrc](const uint8_t *data,
                                        uint16_t length) {
        if (data != nullptr && length != 0)
            aggregateCrc ^= makcm::bridge::crc16Ccitt(data, length);
    };
    addAggregate(rawDeviceDescriptor, rawDeviceDescriptorLength);
    addAggregate(rawConfigurationDescriptor, rawConfigurationDescriptorLength);
    addAggregate(rawBosDescriptor, rawBosDescriptorLength);
    addAggregate(rawQualifierDescriptor, rawQualifierDescriptorLength);
    for (uint8_t i = 0; i < proxyHidDescriptorCount; ++i)
        addAggregate(proxyHidDescriptors[i].data,
                     proxyHidDescriptors[i].receivedLength);
    for (uint8_t i = 0; i < proxyStringDescriptorCount; ++i)
        addAggregate(proxyStringDescriptors[i].data,
                     proxyStringDescriptors[i].receivedLength);
    passthroughProfile.descriptorCrc = aggregateCrc;
    strlcpy(passthroughProfile.manufacturer, device_info.str_desc_manufacturer,
            sizeof(passthroughProfile.manufacturer));
    strlcpy(passthroughProfile.product, device_info.str_desc_product,
            sizeof(passthroughProfile.product));
    strlcpy(passthroughProfile.serial, device_info.str_desc_serial_num,
            sizeof(passthroughProfile.serial));

    // Use the same pure planner on both boards.  The right host must not
    // claim that a topology is usable merely because it managed to parse a
    // few interface descriptors; the left device has the final endpoint
    // resource limits and must receive an explicit reason on rejection.
    makcm::bridge::CapabilityInput capabilityInput;
    capabilityInput.device = rawDeviceDescriptor;
    capabilityInput.deviceLength = rawDeviceDescriptorLength;
    capabilityInput.configuration = rawConfigurationDescriptor;
    capabilityInput.configurationLength = rawConfigurationDescriptorLength;
    capabilityInput.speed = passthroughProfile.usbSpeed;
    capabilityInput.uartBudgetBitsPerSecond = 4000000;
    makcm::bridge::CapabilityResult capability =
        makcm::bridge::planCapability(capabilityInput);
    if (!rawTopologySupported) {
        capability.capability = makcm::bridge::ProxyCapability::Unsupported;
        capability.reason = rawTopologyRejectReason !=
                makcm::bridge::CapabilityReason::None
            ? rawTopologyRejectReason
            : makcm::bridge::CapabilityReason::DescriptorMalformed;
    } else if (!reportDescriptorReady) {
        capability.capability = makcm::bridge::ProxyCapability::Unsupported;
        capability.reason = makcm::bridge::CapabilityReason::BlobIncomplete;
    } else if (passthroughProfile.blobCount >
               makcm::bridge::kMaxProfileBlobs) {
        // ProfileStore has a bounded Blob table.  Reject before sending a
        // partial set instead of letting the left side discover overflow only
        // after D+ has already been considered.
        capability.capability = makcm::bridge::ProxyCapability::Unsupported;
        capability.reason = makcm::bridge::CapabilityReason::BlobIncomplete;
    }
    if (capability.capability == makcm::bridge::ProxyCapability::Unsupported) {
        rawTopologyRejectReason = capability.reason;
    }
    if (capability.capability == makcm::bridge::ProxyCapability::Unsupported) {
        // The left side checks CapabilityResult before parsing the optional
        // blobs, so an unsupported device can be rejected cleanly without
        // manufacturing a partial descriptor set or pulling D+ high.
        passthroughProfile.blobCount = 0;
        passthroughProfile.descriptorCrc = 0;
    }

    if (!sendBridgeFrame(makcm::bridge::MessageType::ProfileBegin, 0,
                         reinterpret_cast<const uint8_t *>(&passthroughProfile),
                         sizeof(passthroughProfile))) {
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(3));

    if (capability.capability == makcm::bridge::ProxyCapability::Unsupported) {
        sendBridgeFrame(makcm::bridge::MessageType::CapabilityResult, 0,
                        reinterpret_cast<const uint8_t *>(&capability),
                        sizeof(capability));
        sendBridgeFrame(makcm::bridge::MessageType::ProfileReady, 0, nullptr, 0);
        serial1Send("USB_PROFILE_UNSUPPORTED reason=%u\n",
                    static_cast<unsigned>(capability.reason));
        return;
    }

    auto sendBlob = [this](
                        makcm::bridge::ProfileBlobType blobType,
                        uint16_t key, uint16_t langid,
                        const uint8_t *descriptor,
                        uint16_t descriptorLength) -> bool {
        if (descriptor == nullptr || descriptorLength == 0 ||
            descriptorLength > makcm::bridge::kMaxProfileBlobSize) {
            return false;
        }
        const uint16_t crc = makcm::bridge::crc16Ccitt(
            descriptor, descriptorLength);
        uint16_t offset = 0;
        while (offset < descriptorLength) {
            uint8_t chunk[makcm::bridge::kMaxPayloadSize] = {};
            const uint16_t count = std::min<uint16_t>(
                makcm::bridge::kMaxPayloadSize -
                    sizeof(makcm::bridge::ProfileBlobChunkHeader),
                descriptorLength - offset);
            makcm::bridge::ProfileBlobChunkHeader header;
            header.type = blobType;
            header.key = key;
            header.langid = langid;
            header.offset = offset;
            header.totalLength = descriptorLength;
            header.totalCrc = crc;
            memcpy(chunk, &header, sizeof(header));
            memcpy(chunk + sizeof(header), descriptor + offset, count);
            if (!sendBridgeFrame(makcm::bridge::MessageType::ProfileBlobChunk,
                                 offset, chunk,
                                 static_cast<uint16_t>(sizeof(header) + count))) {
                return false;
            }
            offset += count;
            vTaskDelay(pdMS_TO_TICKS(3));
        }
        return true;
    };
    if (!sendBlob(makcm::bridge::ProfileBlobType::Device, 0, 0,
                  rawDeviceDescriptor, rawDeviceDescriptorLength) ||
        !sendBlob(makcm::bridge::ProfileBlobType::Configuration, 0, 0,
                  rawConfigurationDescriptor, rawConfigurationDescriptorLength)) {
        return;
    }

    if (rawBosDescriptorLength != 0 &&
        !sendBlob(makcm::bridge::ProfileBlobType::Bos, 0, 0,
                  rawBosDescriptor, rawBosDescriptorLength)) return;
    if (rawQualifierDescriptorLength != 0 &&
        !sendBlob(makcm::bridge::ProfileBlobType::DeviceQualifier, 0, 0,
                  rawQualifierDescriptor, rawQualifierDescriptorLength)) return;

    for (uint8_t descriptorIndex = 0;
         descriptorIndex < proxyHidDescriptorCount; ++descriptorIndex) {
        const ProxyHidDescriptor &descriptor =
            proxyHidDescriptors[descriptorIndex];
        if (!sendBlob(makcm::bridge::ProfileBlobType::HidReport,
                      descriptor.interfaceNumber, 0, descriptor.data,
                      descriptor.receivedLength)) return;
    }

    for (uint8_t descriptorIndex = 0;
         descriptorIndex < proxyStringDescriptorCount; ++descriptorIndex) {
        const ProxyStringDescriptor &descriptor =
            proxyStringDescriptors[descriptorIndex];
        // Index EE is an optional Microsoft OS probe.  A STALL is represented
        // by an empty slot and must not turn an otherwise valid profile into
        // a partial Blob set.
        if (descriptor.receivedLength == 0) continue;
        if (!sendBlob(makcm::bridge::ProfileBlobType::String,
                      descriptor.index, descriptor.langid, descriptor.data,
                      descriptor.receivedLength)) return;
    }
    sendBridgeFrame(makcm::bridge::MessageType::CapabilityResult, 0,
                    reinterpret_cast<const uint8_t *>(&capability),
                    sizeof(capability));
    sendControlCache();
    sendBridgeFrame(makcm::bridge::MessageType::ProfileReady, 0, nullptr, 0);
}

void EspUsbHost::sendControlCache()
{
    constexpr uint16_t headerLength =
        sizeof(makcm::bridge::ProfileControlCacheHeader);
    for (uint8_t i = 0; i < proxyControlCacheCount; ++i) {
        const ProxyControlCache &entry = proxyControlCache[i];
        if (entry.receivedLength == 0 || entry.wLength == 0 ||
            entry.receivedLength != entry.wLength) {
            continue;
        }
        uint16_t offset = 0;
        while (offset < entry.receivedLength) {
            uint8_t payload[makcm::bridge::kMaxPayloadSize] = {};
            const uint16_t count = std::min<uint16_t>(
                makcm::bridge::kMaxPayloadSize - headerLength,
                entry.receivedLength - offset);
            makcm::bridge::ProfileControlCacheHeader header;
            header.bmRequestType = entry.bmRequestType;
            header.bRequest = entry.bRequest;
            header.wValue = entry.wValue;
            header.wIndex = entry.wIndex;
            header.wLength = entry.wLength;
            header.offset = offset;
            header.totalLength = entry.receivedLength;
            memcpy(payload, &header, sizeof(header));
            memcpy(payload + headerLength, entry.data + offset, count);
            if (!sendBridgeFrame(
                    makcm::bridge::MessageType::ProfileControlCacheChunk,
                    offset, payload,
                    static_cast<uint16_t>(headerLength + count))) {
                ESP_LOGE("EspUsbHost", "Failed to send control cache chunk");
                return;
            }
            offset = static_cast<uint16_t>(offset + count);
            vTaskDelay(pdMS_TO_TICKS(3));
        }
    }
}

bool EspUsbHost::submitOutputReport(const uint8_t *data, uint16_t length)
{
    if (data == nullptr || length == 0 || selectedOutEndpoint == 0 ||
        deviceHandle == nullptr || length > selectedOutPacketSize) {
        return false;
    }
    const uint8_t physicalEndpoint =
        proxyToPhysicalEndpoint(selectedOutEndpoint);
    if (physicalEndpoint == 0) return false;
    usb_transfer_t *transfer = nullptr;
    if (usb_host_transfer_alloc(selectedOutPacketSize, 0, &transfer) != ESP_OK) {
        return false;
    }
    memcpy(transfer->data_buffer, data, length);
    transfer->device_handle = deviceHandle;
    transfer->bEndpointAddress = physicalEndpoint;
    transfer->num_bytes = length;
    transfer->callback = [](usb_transfer_t *completed) {
        usb_host_transfer_free(completed);
    };
    transfer->context = this;
    if (usb_host_transfer_submit(transfer) != ESP_OK) {
        usb_host_transfer_free(transfer);
        return false;
    }
    return true;
}

bool EspUsbHost::submitEndpointOutput(uint8_t endpoint, const uint8_t *data,
                                      uint16_t length)
{
    if (data == nullptr || length == 0 || deviceHandle == nullptr ||
        (endpoint & 0x80) != 0) {
        return false;
    }
    const ProxyEndpoint *route = nullptr;
    for (uint8_t i = 0; i < proxyEndpointCount; ++i) {
        if (proxyEndpoints[i].address == endpoint) {
            route = &proxyEndpoints[i];
            break;
        }
    }
    if (route == nullptr || length > route->packetSize) {
        return false;
    }
    usb_transfer_t *transfer = nullptr;
    if (usb_host_transfer_alloc(route->packetSize, 0, &transfer) != ESP_OK) {
        return false;
    }
    memcpy(transfer->data_buffer, data, length);
    transfer->device_handle = deviceHandle;
    transfer->bEndpointAddress = route->physicalAddress;
    if (transfer->bEndpointAddress == 0) {
        usb_host_transfer_free(transfer);
        return false;
    }
    transfer->num_bytes = length;
    transfer->callback = [](usb_transfer_t *completed) {
        usb_host_transfer_free(completed);
    };
    transfer->context = this;
    if (usb_host_transfer_submit(transfer) != ESP_OK) {
        usb_host_transfer_free(transfer);
        return false;
    }
    return true;
}

bool EspUsbHost::submitProxyControl(
    const makcm::bridge::ControlRequestHeader &request,
    const uint8_t *data, uint16_t length, uint32_t session)
{
    constexpr uint16_t maxResponseData =
        makcm::bridge::kMaxPayloadSize -
        sizeof(makcm::bridge::ControlResponseHeader);
    const bool directionIn = (request.bmRequestType & 0x80) != 0;
    const bool valid = deviceHandle != nullptr &&
        request.dataLength == length &&
        ((directionIn && length == 0 && request.wLength <= maxResponseData) ||
         (!directionIn && request.wLength == length));

    auto sendImmediateResponse = [this, &request, session](
                                     makcm::bridge::ControlStatus status) {
        makcm::bridge::ControlResponseHeader response;
        response.requestId = request.requestId;
        response.status = status;
        response.dataLength = 0;
        sendBridgeFrame(makcm::bridge::MessageType::ControlInResult,
                        request.requestId,
                        reinterpret_cast<const uint8_t *>(&response),
                        sizeof(response), session);
    };
    if (!valid || (!directionIn && length != 0 && data == nullptr)) {
        sendImmediateResponse(makcm::bridge::ControlStatus::Error);
        return false;
    }

    // The upstream profile uses compact proxy interface/endpoint numbers.
    // Translate recipients back to the physical DualSense topology before
    // sending EP0.  A removed audio object is explicitly stalled so a
    // controller request cannot be redirected to an unrelated endpoint.
    makcm::bridge::ControlRequestHeader physicalRequest = request;
    if (dualSenseControllerAdapter) {
        const uint8_t recipient = request.bmRequestType & 0x1F;
        if (recipient == 1) {
            const uint8_t proxyInterface =
                static_cast<uint8_t>(request.wIndex & 0xFF);
            const uint8_t physicalInterface =
                proxyToPhysicalInterface(proxyInterface);
            if (physicalInterface == 0xFF) {
                sendImmediateResponse(makcm::bridge::ControlStatus::Stalled);
                return false;
            }
            physicalRequest.wIndex = static_cast<uint16_t>(
                (request.wIndex & 0xFF00) | physicalInterface);
        } else if (recipient == 2) {
            const uint8_t proxyEndpoint =
                static_cast<uint8_t>(request.wIndex & 0xFF);
            const uint8_t physicalEndpoint =
                proxyToPhysicalEndpoint(proxyEndpoint);
            if (physicalEndpoint == 0) {
                sendImmediateResponse(makcm::bridge::ControlStatus::Stalled);
                return false;
            }
            physicalRequest.wIndex = static_cast<uint16_t>(
                (request.wIndex & 0xFF00) | physicalEndpoint);
        }
    }

    const uint16_t transferDataLength = directionIn ? request.wLength : length;
    usb_transfer_t *transfer = nullptr;
    if (usb_host_transfer_alloc(8 + transferDataLength, 0, &transfer) != ESP_OK) {
        sendImmediateResponse(makcm::bridge::ControlStatus::Error);
        return false;
    }
    ProxyControlContext *context = new (std::nothrow) ProxyControlContext;
    if (context == nullptr) {
        usb_host_transfer_free(transfer);
        sendImmediateResponse(makcm::bridge::ControlStatus::Error);
        return false;
    }
    context->host = this;
    context->requestId = request.requestId;
    context->session = session;
    context->directionIn = directionIn;
    context->expectedLength = request.wLength;
    context->ep0PacketSize = std::max<uint8_t>(
        descriptor_device.bMaxPacketSize0, 1);

    uint8_t *setup = transfer->data_buffer;
    setup[0] = physicalRequest.bmRequestType;
    setup[1] = physicalRequest.bRequest;
    setup[2] = static_cast<uint8_t>(physicalRequest.wValue);
    setup[3] = static_cast<uint8_t>(physicalRequest.wValue >> 8);
    setup[4] = static_cast<uint8_t>(physicalRequest.wIndex);
    setup[5] = static_cast<uint8_t>(physicalRequest.wIndex >> 8);
    setup[6] = static_cast<uint8_t>(physicalRequest.wLength);
    setup[7] = static_cast<uint8_t>(physicalRequest.wLength >> 8);
    if (!directionIn && length != 0) {
        memcpy(setup + 8, data, length);
    }
    transfer->device_handle = deviceHandle;
    transfer->bEndpointAddress = 0;
    transfer->num_bytes = 8 + transferDataLength;
    transfer->callback = _onProxyControl;
    transfer->context = context;
    if (usb_host_transfer_submit_control(clientHandle, transfer) != ESP_OK) {
        delete context;
        usb_host_transfer_free(transfer);
        sendImmediateResponse(makcm::bridge::ControlStatus::Error);
        return false;
    }
    return true;
}

void EspUsbHost::_onProxyControl(usb_transfer_t *transfer)
{
    ProxyControlContext *context =
        static_cast<ProxyControlContext *>(transfer->context);
    if (context == nullptr || context->host == nullptr) {
        delete context;
        usb_host_transfer_free(transfer);
        return;
    }
    EspUsbHost *host = context->host;
    uint8_t payload[makcm::bridge::kMaxPayloadSize] = {};
    makcm::bridge::ControlResponseHeader response;
    response.requestId = context->requestId;
    if (transfer->status == USB_TRANSFER_STATUS_COMPLETED) {
        response.status = makcm::bridge::ControlStatus::Completed;
    } else if (transfer->status == USB_TRANSFER_STATUS_STALL) {
        response.status = makcm::bridge::ControlStatus::Stalled;
    } else {
        response.status = makcm::bridge::ControlStatus::Error;
    }
    if (response.status == makcm::bridge::ControlStatus::Completed &&
        context->directionIn) {
        if (transfer->actual_num_bytes > 8) {
            response.dataLength = std::min<uint16_t>(
                transfer->actual_num_bytes - 8,
                makcm::bridge::kMaxPayloadSize - sizeof(response));
        }
        if (response.dataLength < context->expectedLength) {
            response.flags |= makcm::bridge::kControlFlagShortPacket;
            if (response.dataLength != 0 &&
                (response.dataLength % context->ep0PacketSize) == 0) {
                // A short response ending exactly on an EP0 max packet needs
                // a terminating ZLP.  A response that reaches wLength does
                // not need this marker even when it is packet-aligned.
                response.flags |= makcm::bridge::kControlFlagZlpRequired;
            }
        }
    }
    memcpy(payload, &response, sizeof(response));
    if (response.dataLength != 0) {
        memcpy(payload + sizeof(response), transfer->data_buffer + 8,
               response.dataLength);
    }
    host->sendBridgeFrame(makcm::bridge::MessageType::ControlInResult,
                          context->requestId, payload,
                          sizeof(response) + response.dataLength,
                          context->session);
    delete context;
    usb_host_transfer_free(transfer);
}

bool EspUsbHost::submitFeatureSet(const uint8_t *data, uint16_t length)
{
    if (data == nullptr || length == 0 || deviceHandle == nullptr ||
        selectedInterface == 0xFF) {
        return false;
    }
    const uint8_t physicalInterface =
        proxyToPhysicalInterface(selectedInterface);
    if (physicalInterface == 0xFF) return false;
    const uint8_t reportId = data[0];
    const uint16_t payloadLength = length - 1;
    usb_transfer_t *transfer = nullptr;
    if (usb_host_transfer_alloc(8 + payloadLength, 0, &transfer) != ESP_OK) {
        return false;
    }
    uint8_t *setup = transfer->data_buffer;
    setup[0] = 0x21;
    setup[1] = 0x09;
    setup[2] = reportId;
    setup[3] = 0x03;
    setup[4] = physicalInterface;
    setup[5] = 0;
    setup[6] = static_cast<uint8_t>(payloadLength);
    setup[7] = static_cast<uint8_t>(payloadLength >> 8);
    if (payloadLength != 0) {
        memcpy(setup + 8, data + 1, payloadLength);
    }
    transfer->device_handle = deviceHandle;
    transfer->bEndpointAddress = 0;
    transfer->num_bytes = 8 + payloadLength;
    transfer->callback = [](usb_transfer_t *completed) {
        usb_host_transfer_free(completed);
    };
    transfer->context = this;
    if (usb_host_transfer_submit_control(clientHandle, transfer) != ESP_OK) {
        usb_host_transfer_free(transfer);
        return false;
    }
    return true;
}

bool EspUsbHost::submitFeatureGet(uint8_t reportId, uint16_t length)
{
    length = std::min<uint16_t>(length, 64);
    if (deviceHandle == nullptr || selectedInterface == 0xFF || length == 0) {
        return false;
    }
    const uint8_t physicalInterface =
        proxyToPhysicalInterface(selectedInterface);
    if (physicalInterface == 0xFF) return false;
    usb_transfer_t *transfer = nullptr;
    if (usb_host_transfer_alloc(8 + length, 0, &transfer) != ESP_OK) {
        return false;
    }
    FeatureGetContext *context = new (std::nothrow) FeatureGetContext;
    if (context == nullptr) {
        usb_host_transfer_free(transfer);
        return false;
    }
    context->host = this;
    context->session = activeSessionId;
    uint8_t *setup = transfer->data_buffer;
    setup[0] = 0xA1;
    setup[1] = 0x01;
    setup[2] = reportId;
    setup[3] = 0x03;
    setup[4] = physicalInterface;
    setup[5] = 0;
    setup[6] = static_cast<uint8_t>(length);
    setup[7] = static_cast<uint8_t>(length >> 8);
    transfer->device_handle = deviceHandle;
    transfer->bEndpointAddress = 0;
    transfer->num_bytes = 8 + length;
    transfer->callback = _onFeatureGet;
    transfer->context = context;
    if (usb_host_transfer_submit_control(clientHandle, transfer) != ESP_OK) {
        delete context;
        usb_host_transfer_free(transfer);
        return false;
    }
    return true;
}

void EspUsbHost::_onFeatureGet(usb_transfer_t *transfer)
{
    FeatureGetContext *context =
        static_cast<FeatureGetContext *>(transfer->context);
    EspUsbHost *host = context != nullptr ? context->host : nullptr;
    if (host != nullptr && transfer->status == USB_TRANSFER_STATUS_COMPLETED &&
        transfer->actual_num_bytes >= 8) {
        uint8_t response[65] = {};
        response[0] = transfer->data_buffer[2];
        const uint16_t payloadLength = std::min<uint16_t>(
            transfer->actual_num_bytes - 8, sizeof(response) - 1);
        memcpy(response + 1, transfer->data_buffer + 8, payloadLength);
        host->sendBridgeFrame(makcm::bridge::MessageType::HidFeatureResponse,
                              0, response, payloadLength + 1,
                              context->session);
    }
    delete context;
    usb_host_transfer_free(transfer);
}
