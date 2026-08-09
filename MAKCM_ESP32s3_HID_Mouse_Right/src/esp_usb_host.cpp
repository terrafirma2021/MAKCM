#include "EspUsbHost.h"
#include <algorithm>
#include <cstdio>
#include <new>

namespace {
// Microsoft OS 1.0 uses the otherwise unreferenced string index 0xEE as the
// feature-discovery handshake for compatible IDs such as MS_COMP_XUSB10.
constexpr uint8_t kMicrosoftOsStringIndex = 0xEE;

struct ProxyOsControlContext {
    EspUsbHost *host = nullptr;
    uint8_t slotIndex = 0;
};

struct OptionalDescriptorContext {
    EspUsbHost *host = nullptr;
    makcm::bridge::ProfileBlobType type =
        makcm::bridge::ProfileBlobType::Unknown;
};

// usb_host_get_active_config_descriptor() returns a pointer into the host
// library's cache.  Treat the bytes as untrusted until the header and every
// child descriptor agree with the advertised wTotalLength.  In particular,
// never copy a claimed length from a malformed/short cache entry: that would
// both over-read the cache and send a half configuration to the left board.
bool validateConfigurationDescriptor(const uint8_t *data, uint16_t available,
                                     uint16_t &declared_length,
                                     uint8_t &interface_count) {
    declared_length = 0;
    interface_count = 0;
    if (data == nullptr || available < 9 || data[0] != 9 || data[1] != 2)
        return false;
    declared_length = static_cast<uint16_t>(data[2]) |
                      (static_cast<uint16_t>(data[3]) << 8);
    interface_count = data[4];
    if (declared_length < 9 || declared_length != available)
        return false;

    uint16_t offset = 0;
    while (offset < declared_length) {
        if (offset + 2 > declared_length) return false;
        const uint8_t length = data[offset];
        if (length < 2 || offset + length > declared_length) return false;
        if (offset == 0 && length != 9) return false;
        offset = static_cast<uint16_t>(offset + length);
    }
    return offset == declared_length;
}
}
#include "freertos/semphr.h"

bool EspUsbHost::deviceMouseReady = false;
bool EspUsbHost::deviceConnected = false;
EspUsbHost::HIDReportDescriptor EspUsbHost::HIDReportDesc = {};
void flashLED();

uint8_t EspUsbHost::proxyToPhysicalEndpoint(uint8_t endpoint) const {
    if (!dualSenseControllerAdapter) return endpoint;
    return makcm::bridge::dualSenseMapPhysicalEndpoint(dualSenseTransform,
                                                       endpoint);
}

uint8_t EspUsbHost::physicalToProxyEndpoint(uint8_t endpoint) const {
    if (!dualSenseControllerAdapter) return endpoint;
    return makcm::bridge::dualSenseMapEndpoint(dualSenseTransform, endpoint);
}

uint8_t EspUsbHost::proxyToPhysicalInterface(uint8_t interfaceNumber) const {
    if (!dualSenseControllerAdapter) return interfaceNumber;
    return makcm::bridge::dualSenseMapPhysicalInterface(dualSenseTransform,
                                                         interfaceNumber);
}


void usbLibraryTask(void *arg)
{
    EspUsbHost *instance = static_cast<EspUsbHost *>(arg);

    while (true)
    {
        uint32_t event_flags;
        esp_err_t err = usb_host_lib_handle_events(portMAX_DELAY, &event_flags);

        if (err != ESP_OK)
        {
            ESP_LOGE("EspUsbHost", "usb_host_lib_handle_events() err=%x", err);
            continue;
        }

        if (instance->clientHandle == NULL || !instance->isClientRegistering)
        {
            ESP_LOGI("EspUsbHost", "Registering client...");
            const usb_host_client_config_t client_config = {
                .max_num_event_msg = 10,
                .async = {
                    .client_event_callback = instance->_clientEventCallback,
                    .callback_arg = instance,
                }};

            err = usb_host_client_register(&client_config, &instance->clientHandle);
            ESP_LOGI("EspUsbHost", "usb_host_client_register() status: %d", err);
            if (err != ESP_OK)
            {
                ESP_LOGW("EspUsbHost", "Failed to re-register client, retrying...");
                vTaskDelay(100);
            }
            else
            {
                ESP_LOGI("EspUsbHost", "Client registered successfully.");
                instance->isClientRegistering = true;
            }
        }
    }
}

void usbClientTask(void *arg)
{
    EspUsbHost *instance = static_cast<EspUsbHost *>(arg);

    while (true)
    {
        if (!instance->isClientRegistering)
        {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        usb_host_client_handle_events(instance->clientHandle, portMAX_DELAY);
    }
}



void EspUsbHost::get_device_status()
{
    const char *TAG = "get_device_status";

    if (!EspUsbHost::deviceConnected)
    {
        return;
    }

    usb_transfer_t *transfer;
    esp_err_t err = usb_host_transfer_alloc(8 + 2, 0, &transfer);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "usb_host_transfer_alloc() err=%X", err);
        return;
    }

    transfer->num_bytes = 8 + 2;
    transfer->data_buffer[0] = USB_BM_REQUEST_TYPE_DIR_IN | USB_BM_REQUEST_TYPE_TYPE_STANDARD | USB_BM_REQUEST_TYPE_RECIP_DEVICE;
    transfer->data_buffer[1] = USB_B_REQUEST_GET_STATUS;
    transfer->data_buffer[2] = 0x00;
    transfer->data_buffer[3] = 0x00;
    transfer->data_buffer[4] = 0x00;
    transfer->data_buffer[5] = 0x00;
    transfer->data_buffer[6] = 0x02;
    transfer->data_buffer[7] = 0x00;

    transfer->device_handle = deviceHandle;
    transfer->bEndpointAddress = 0x00;

    transfer->callback = [](usb_transfer_t *transfer)
    {
        const char *TAG = "get_device_status_callback";
        EspUsbHost *usbHost = static_cast<EspUsbHost *>(transfer->context);

        if (transfer->status == USB_TRANSFER_STATUS_COMPLETED)
        {
            uint16_t status = (transfer->data_buffer[9] << 8) | transfer->data_buffer[8];
            ESP_LOGI(TAG, "Device status: %X", status);

            // USB device GET_STATUS bit 1 reports whether remote wakeup is
            // enabled. This is observation only; the proxy never infers or
            // changes downstream power state from input inactivity.
            if (status & (1u << 1))
            {
                ESP_LOGI(TAG, "Remote Wakeup is enabled.");
            }
            else
            {
                ESP_LOGI(TAG, "Remote Wakeup is disabled.");
            }
        }
        else
        {
            ESP_LOGE(TAG, "GET_STATUS transfer failed with status=%X", transfer->status);
        }

        usb_host_transfer_free(transfer);
    };

    transfer->context = this;

    err = usb_host_transfer_submit_control(clientHandle, transfer);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "usb_host_transfer_submit_control() err=%X", err);
        usb_host_transfer_free(transfer);
    }
}

String EspUsbHost::getUsbDescString(const usb_str_desc_t *str_desc)
{
    String str = "";
    if (str_desc == NULL)
    {
        return str;
    }

    for (int i = 0; i < str_desc->bLength / 2; i++)
    {
        if (str_desc->wData[i] > 0xFF)
        {
            continue;
        }
        str += char(str_desc->wData[i]);
    }
    return str;
}


void EspUsbHost::logRawBytes(const char *functionName, const uint8_t *data, uint16_t length)
{
    if (functionName == nullptr || data == nullptr || length == 0) return;

    // This function is called from usbClientTask while it owns the host
    // callback.  std::stringstream/iomanip used several kilobytes of stack
    // and heap on the cold-enumeration path and could reset the MCU directly
    // after NEW_DEV.  Keep diagnostic formatting bounded and DEBUG-only.
    constexpr uint16_t kBytesPerLine = 24;
    for (uint16_t offset = 0; offset < length; offset += kBytesPerLine) {
        char line[kBytesPerLine * 3 + 1] = {};
        size_t used = 0;
        const uint16_t count = std::min<uint16_t>(
            kBytesPerLine, static_cast<uint16_t>(length - offset));
        for (uint16_t i = 0; i < count && used + 3 < sizeof(line); ++i) {
            const int written = snprintf(line + used, sizeof(line) - used,
                                         "%02X ", data[offset + i]);
            if (written <= 0) break;
            used += static_cast<size_t>(written);
        }
        ESP_LOGD(functionName, "Raw Bytes off=%u: %s",
                 static_cast<unsigned>(offset), line);
    }
}

void EspUsbHost::onConfig(const uint8_t bDescriptorType, const uint8_t *p)
{
        logRawBytes("EspUsbHost::onConfig", p, p[0]);

    switch (bDescriptorType)
    {
    case USB_B_DESCRIPTOR_TYPE_DEVICE:
    {
        // Log raw bytes for USB_DEVICE_DESC
        logRawBytes("USB_DEVICE_DESC", p, p[0]); 
        ESP_LOGI("EspUsbHost::onConfig", "Descriptor Type: USB_DEVICE_DESC");

        const usb_device_desc_t *dev_desc = (const usb_device_desc_t *)p;

        // Assign fields to ensure type compatibility
        descriptor_device.bLength = dev_desc->bLength;
        descriptor_device.bDescriptorType = dev_desc->bDescriptorType;
        descriptor_device.bcdUSB = dev_desc->bcdUSB;
        descriptor_device.bDeviceClass = dev_desc->bDeviceClass;
        descriptor_device.bDeviceSubClass = dev_desc->bDeviceSubClass;
        descriptor_device.bDeviceProtocol = dev_desc->bDeviceProtocol;
        descriptor_device.bMaxPacketSize0 = dev_desc->bMaxPacketSize0;
        descriptor_device.idVendor = dev_desc->idVendor;
        descriptor_device.idProduct = dev_desc->idProduct;
        descriptor_device.bcdDevice = dev_desc->bcdDevice;
        descriptor_device.iManufacturer = dev_desc->iManufacturer;
        descriptor_device.iProduct = dev_desc->iProduct;
        descriptor_device.iSerialNumber = dev_desc->iSerialNumber;
        descriptor_device.bNumConfigurations = dev_desc->bNumConfigurations;

        break;
    }

    case USB_B_DESCRIPTOR_TYPE_STRING:
    {
        // Log raw bytes for USB_STRING_DESC
        logRawBytes("USB_STRING_DESC", p, p[0]);
        ESP_LOGI("EspUsbHost::onConfig", "Descriptor Type: USB_STRING_DESC");

        const usb_standard_desc_t *desc = (const usb_standard_desc_t *)p;
        usb_string_descriptor_t usbStringDescriptor;
        usbStringDescriptor.bLength = desc->bLength;
        usbStringDescriptor.bDescriptorType = desc->bDescriptorType;
        usbStringDescriptor.data = "";

        for (int i = 0; i < (desc->bLength - 2); i++)
        {
            if (desc->val[i] < 16)
            {
                usbStringDescriptor.data += "0";
            }
            usbStringDescriptor.data += String(desc->val[i], HEX) + " ";
        }

        break;
    }

    case USB_B_DESCRIPTOR_TYPE_INTERFACE:
    {
        // Log raw bytes for USB_INTERFACE_DESC
        logRawBytes("USB_INTERFACE_DESC", p, p[0]);
        ESP_LOGI("EspUsbHost::onConfig", "Descriptor Type: USB_INTERFACE_DESC");

        const usb_intf_desc_t *intf = (const usb_intf_desc_t *)p;

        if (interfaceCounter < MAX_INTERFACE_DESCRIPTORS)
        {
            interface_descriptors[interfaceCounter].bLength = intf->bLength;
            interface_descriptors[interfaceCounter].bDescriptorType = intf->bDescriptorType;
            interface_descriptors[interfaceCounter].bInterfaceNumber = intf->bInterfaceNumber;
            interface_descriptors[interfaceCounter].bAlternateSetting = intf->bAlternateSetting;
            interface_descriptors[interfaceCounter].bNumEndpoints = intf->bNumEndpoints;
            interface_descriptors[interfaceCounter].bInterfaceClass = intf->bInterfaceClass;
            interface_descriptors[interfaceCounter].bInterfaceSubClass = intf->bInterfaceSubClass;
            interface_descriptors[interfaceCounter].bInterfaceProtocol = intf->bInterfaceProtocol;
            interface_descriptors[interfaceCounter].iInterface = intf->iInterface;

            currentInterfaceNumber = intf->bInterfaceNumber;
            currentPhysicalInterfaceNumber =
                proxyToPhysicalInterface(intf->bInterfaceNumber);
            currentInterfaceClaimed = false;
            currentInterfaceIsAuxMouse = false;

            if (selectedInterface == 0xFF) {
                selectedInterface = intf->bInterfaceNumber;
                passthroughProfile.interfaceNumber = intf->bInterfaceNumber;
                passthroughProfile.interfaceClass = intf->bInterfaceClass;
                passthroughProfile.interfaceSubClass = intf->bInterfaceSubClass;
                passthroughProfile.interfaceProtocol = intf->bInterfaceProtocol;
            }
            const bool shouldClaim = intf->bAlternateSetting == 0;
            if (intf->bAlternateSetting != 0) {
                rawTopologySupported = false;
                rawTopologyRejectReason =
                    makcm::bridge::CapabilityReason::AlternateSettingUnsupported;
            }
            if (!shouldClaim)
            {
                ESP_LOGI("EspUsbHost", "Skipping alternate interface %u (class 0x%02x)",
                         intf->bInterfaceNumber, intf->bInterfaceClass);
            }
            else if (intf->bNumEndpoints == 0)
            {
                // A composite device may contain a control-only/vendor
                // interface (the target XInput controller's security
                // interface is one example).  It has no data endpoint to
                // claim, and ESP-IDF can legitimately report a claim error
                // when another class layer owns that interface.  The raw
                // configuration blob and EP0 requests can still be proxied,
                // so a failed claim here must not reject the whole parent
                // device.  Only interfaces that carry data endpoints need a
                // successful host claim below.
                currentInterfaceClaimed = false;
                ESP_LOGI("EspUsbHost",
                         "Leaving control-only interface %u unclaimed",
                         intf->bInterfaceNumber);
            }
            else
            {
                if (currentPhysicalInterfaceNumber == 0xFF) {
                    rawTopologySupported = false;
                    rawTopologyRejectReason =
                        makcm::bridge::CapabilityReason::DescriptorMalformed;
                    currentInterfaceClaimed = false;
                    break;
                }
                this->claim_err = usb_host_interface_claim(this->clientHandle, this->deviceHandle,
                                                            currentPhysicalInterfaceNumber,
                                                            intf->bAlternateSetting);
                if (this->claim_err != ESP_OK)
                {
                    ESP_LOGE("EspUsbHost", "usb_host_interface_claim() err=%x", this->claim_err);
                    rawTopologySupported = false;
                    rawTopologyRejectReason =
                        makcm::bridge::CapabilityReason::DescriptorMalformed;
                }
                else
                {
                    currentInterfaceClaimed = true;
                    ESP_LOGI("EspUsbHost", "usb_host_interface_claim() successful for interface %d", intf->bInterfaceNumber);
                    if (this->usbInterfaceSize < sizeof(this->usbInterface)) {
                        this->usbInterface[this->usbInterfaceSize++] =
                            currentPhysicalInterfaceNumber;
                    }

                    if (currentInterfaceNumber < 17) {
                        endpoint_data_list[currentInterfaceNumber].bInterfaceNumber = intf->bInterfaceNumber;
                        endpoint_data_list[currentInterfaceNumber].bInterfaceClass = intf->bInterfaceClass;
                        endpoint_data_list[currentInterfaceNumber].bInterfaceSubClass = intf->bInterfaceSubClass;
                        endpoint_data_list[currentInterfaceNumber].bInterfaceProtocol = intf->bInterfaceProtocol;
                        endpoint_data_list[currentInterfaceNumber].bCountryCode = 0; // FIX THIS !!
                    }
                }
            }

            interfaceCounter++;
        }

        break;
    }

    case USB_B_DESCRIPTOR_TYPE_ENDPOINT:
    {
        // Log raw bytes for USB_ENDPOINT_DESC
        logRawBytes("USB_ENDPOINT_DESC", p, p[0]);
        ESP_LOGI("EspUsbHost::onConfig", "Descriptor Type: USB_ENDPOINT_DESC");

        const usb_ep_desc_t *ep_desc = (const usb_ep_desc_t *)p;

        if (endpointCounter < MAX_ENDPOINT_DESCRIPTORS)
        {
            endpoint_descriptors[endpointCounter].bLength = ep_desc->bLength;
            endpoint_descriptors[endpointCounter].bDescriptorType = ep_desc->bDescriptorType;
            endpoint_descriptors[endpointCounter].bEndpointAddress = ep_desc->bEndpointAddress;
            endpoint_descriptors[endpointCounter].endpointID = USB_EP_DESC_GET_EP_NUM(ep_desc);
            endpoint_descriptors[endpointCounter].direction = USB_EP_DESC_GET_EP_DIR(ep_desc) ? "IN" : "OUT";
            endpoint_descriptors[endpointCounter].bmAttributes = ep_desc->bmAttributes;
            endpoint_descriptors[endpointCounter].attributes =
                (ep_desc->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) == USB_BM_ATTRIBUTES_XFER_CONTROL ? "CTRL" :
                (ep_desc->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) == USB_BM_ATTRIBUTES_XFER_ISOC ? "ISOC" :
                (ep_desc->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) == USB_BM_ATTRIBUTES_XFER_BULK ? "BULK" :
                "Interrupt";

            endpoint_descriptors[endpointCounter].wMaxPacketSize = ep_desc->wMaxPacketSize;
            endpoint_descriptors[endpointCounter].bInterval = ep_desc->bInterval;

            if (!currentInterfaceClaimed)
            {
                ESP_LOGI("EspUsbHost", "Skipping endpoint 0x%02x on unclaimed interface %u",
                         ep_desc->bEndpointAddress, currentInterfaceNumber);
                endpointCounter++;
                break;
            }

            uint8_t ep_num = USB_EP_DESC_GET_EP_NUM(ep_desc);
            if (ep_num < 17 && currentInterfaceNumber < 17) {
                endpoint_data_list[ep_num].bInterfaceNumber = endpoint_data_list[currentInterfaceNumber].bInterfaceNumber;
                endpoint_data_list[ep_num].bInterfaceClass = endpoint_data_list[currentInterfaceNumber].bInterfaceClass;
                endpoint_data_list[ep_num].bInterfaceSubClass = endpoint_data_list[currentInterfaceNumber].bInterfaceSubClass;
                endpoint_data_list[ep_num].bInterfaceProtocol = endpoint_data_list[currentInterfaceNumber].bInterfaceProtocol;
                endpoint_data_list[ep_num].bCountryCode = endpoint_data_list[currentInterfaceNumber].bCountryCode;
            }

            const bool isInterrupt =
                (ep_desc->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) == USB_BM_ATTRIBUTES_XFER_INT;
            const bool isBulk =
                (ep_desc->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) == USB_BM_ATTRIBUTES_XFER_BULK;
            const bool isIn =
                (ep_desc->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK) != 0;

            if ((!isInterrupt && !isBulk) || ep_num == 0 || ep_num > 6 ||
                ep_desc->wMaxPacketSize == 0 || ep_desc->wMaxPacketSize > 64)
            {
                ESP_LOGE("EspUsbHost", "Unsupported transfer type: bmAttributes=%x", ep_desc->bmAttributes);
                rawTopologySupported = false;
                if ((ep_desc->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) ==
                    USB_BM_ATTRIBUTES_XFER_ISOC) {
                    rawTopologyRejectReason =
                        makcm::bridge::CapabilityReason::IsochronousUnsupported;
                } else if (ep_num > 6) {
                    rawTopologyRejectReason =
                        makcm::bridge::CapabilityReason::EndpointNumberOutOfRange;
                } else {
                    rawTopologyRejectReason =
                        makcm::bridge::CapabilityReason::PacketSizeUnsupported;
                }
                endpointCounter++;
                return;
            }

            if (proxyEndpointCount >=
                sizeof(proxyEndpoints) / sizeof(proxyEndpoints[0])) {
                rawTopologySupported = false;
                rawTopologyRejectReason =
                    makcm::bridge::CapabilityReason::TooManyEndpoints;
                endpointCounter++;
                return;
            }
            ProxyEndpoint &route = proxyEndpoints[proxyEndpointCount++];
            route.address = ep_desc->bEndpointAddress;
            route.physicalAddress = proxyToPhysicalEndpoint(route.address);
            route.interfaceNumber = currentInterfaceNumber;
            route.physicalInterfaceNumber = currentPhysicalInterfaceNumber;
            if (route.physicalAddress == 0) {
                rawTopologySupported = false;
                rawTopologyRejectReason =
                    makcm::bridge::CapabilityReason::DescriptorMalformed;
                endpointCounter++;
                return;
            }
            route.attributes = ep_desc->bmAttributes;
            route.packetSize = ep_desc->wMaxPacketSize;
            route.interval = ep_desc->bInterval;
            passthroughProfile.endpointCount = proxyEndpointCount;

            if (isIn)
            {
                if (selectedInEndpoint == 0) {
                    selectedInEndpoint = ep_desc->bEndpointAddress;
                    selectedInPacketSize = ep_desc->wMaxPacketSize;
                    passthroughProfile.inEndpoint = selectedInEndpoint;
                    passthroughProfile.inPacketSize = selectedInPacketSize;
                    passthroughProfile.inInterval = ep_desc->bInterval;
                }
                if (this->usbTransferSize >= (sizeof(this->usbTransfer) / sizeof(this->usbTransfer[0]))) {
                    ESP_LOGE("EspUsbHost", "No free USB transfer slots");
                    rawTopologySupported = false;
                    rawTopologyRejectReason =
                        makcm::bridge::CapabilityReason::TooManyEndpoints;
                    endpointCounter++;
                    break;
                }
                esp_err_t err = usb_host_transfer_alloc(ep_desc->wMaxPacketSize + 1, 0, &this->usbTransfer[this->usbTransferSize]);
                if (err != ESP_OK)
                {
                    this->usbTransfer[this->usbTransferSize] = NULL;
                    ESP_LOGE("EspUsbHost", "usb_host_transfer_alloc() failed with err=%x", err);
                    rawTopologySupported = false;
                    rawTopologyRejectReason =
                        makcm::bridge::CapabilityReason::TooManyEndpoints;
                    return;
                }
                else
                {
                    ESP_LOGI("EspUsbHost", "usb_host_transfer_alloc() successful, data_buffer_size=%d", ep_desc->wMaxPacketSize + 1);
                }

                this->usbTransfer[this->usbTransferSize]->device_handle = this->deviceHandle;
                this->usbTransfer[this->usbTransferSize]->bEndpointAddress =
                    route.physicalAddress;
                this->usbTransfer[this->usbTransferSize]->callback = this->_onReceive;
                this->usbTransfer[this->usbTransferSize]->context = this;
                this->usbTransfer[this->usbTransferSize]->num_bytes = ep_desc->wMaxPacketSize;
                interval = ep_desc->bInterval;
                isReady = true;
                this->usbTransferSize++;

                ESP_LOGI("EspUsbHost", "Submitting transfer for endpoint 0x%x", ep_desc->bEndpointAddress);

                err = usb_host_transfer_submit(this->usbTransfer[this->usbTransferSize - 1]);
                if (err != ESP_OK)
                {
                    ESP_LOGE("EspUsbHost", "usb_host_transfer_submit() failed with err=%x", err);
                    rawTopologySupported = false;
                    rawTopologyRejectReason =
                        makcm::bridge::CapabilityReason::DescriptorMalformed;
                }
            }
            else
            {
                if (selectedOutEndpoint == 0) {
                    selectedOutEndpoint = ep_desc->bEndpointAddress;
                    selectedOutPacketSize = ep_desc->wMaxPacketSize;
                    passthroughProfile.outEndpoint = selectedOutEndpoint;
                    passthroughProfile.outPacketSize = selectedOutPacketSize;
                    passthroughProfile.outInterval = ep_desc->bInterval;
                }
            }

            endpointCounter++;
        }

        break;
    }

    case USB_B_DESCRIPTOR_TYPE_INTERFACE_ASSOCIATION:
    {
        // Log raw bytes for USB_INTERFACE_ASSOC_DESC
        logRawBytes("USB_INTERFACE_ASSOC_DESC", p, p[0]);   // did not log 
        ESP_LOGI("EspUsbHost::onConfig", "Descriptor Type: USB_INTERFACE_ASSOC_DESC");

        const usb_iad_desc_t *iad_desc = (const usb_iad_desc_t *)p;

        descriptor_interface_association.bLength = iad_desc->bLength;
        descriptor_interface_association.bDescriptorType = iad_desc->bDescriptorType;
        descriptor_interface_association.bFirstInterface = iad_desc->bFirstInterface;
        descriptor_interface_association.bInterfaceCount = iad_desc->bInterfaceCount;
        descriptor_interface_association.bFunctionClass = iad_desc->bFunctionClass;
        descriptor_interface_association.bFunctionSubClass = iad_desc->bFunctionSubClass;
        descriptor_interface_association.bFunctionProtocol = iad_desc->bFunctionProtocol;
        descriptor_interface_association.iFunction = iad_desc->iFunction;

        break;
    }

    case makcm::usb::kHidDescriptorType:
    {
        // Descriptor type 0x21 is class-specific, not globally synonymous
        // with HID. XInput uses a different 0x21 block on a vendor interface.
        // The raw configuration is always preserved; only parse the HID
        // layout when the owning interface is actually class 0x03.
        if (currentInterfaceNumber >= 17 ||
            endpoint_data_list[currentInterfaceNumber].bInterfaceClass !=
                USB_CLASS_HID) {
            break;
        }
        // Log raw bytes for USB_HID_DESC
        logRawBytes("USB_HID_DESC", p, p[0]);
        ESP_LOGI("EspUsbHost::onConfig", "Descriptor Type: USB_HID_DESC");

        const auto *hid_desc =
            reinterpret_cast<const makcm::usb::HidClassDescriptor *>(p);

        if (hidDescriptorCounter < MAX_HID_DESCRIPTORS)
        {
            hid_descriptors[hidDescriptorCounter].bLength = hid_desc->length;
            hid_descriptors[hidDescriptorCounter].bDescriptorType = hid_desc->descriptorType;
            hid_descriptors[hidDescriptorCounter].bcdHID = hid_desc->hidVersion;
            hid_descriptors[hidDescriptorCounter].bCountryCode = hid_desc->countryCode;
            hid_descriptors[hidDescriptorCounter].bNumDescriptors = hid_desc->descriptorCount;
            hid_descriptors[hidDescriptorCounter].bReportType = hid_desc->reportDescriptorType;
            hid_descriptors[hidDescriptorCounter].wReportLength = hid_desc->reportDescriptorLength;
            if (currentInterfaceNumber < 17) {
                endpoint_data_list[currentInterfaceNumber].bCountryCode = hid_descriptors[hidDescriptorCounter].bCountryCode;
            }

            if (currentInterfaceClaimed) {
                if (proxyHidDescriptorCount >=
                     makcm::bridge::kMaxHidInterfaces ||
                     hid_descriptors[hidDescriptorCounter].wReportLength == 0 ||
                     hid_descriptors[hidDescriptorCounter].wReportLength >
                         makcm::bridge::kMaxReportDescriptorSize) {
                    rawTopologySupported = false;
                    rawTopologyRejectReason =
                        makcm::bridge::CapabilityReason::BlobIncomplete;
                } else {
                    ProxyHidDescriptor &proxy =
                        proxyHidDescriptors[proxyHidDescriptorCount++];
                    proxy.interfaceNumber = currentInterfaceNumber;
                    proxy.expectedLength =
                        hid_descriptors[hidDescriptorCounter].wReportLength;
                    // `DeviceProfile::reportDescriptorLength` is a legacy
                    // observer field, not the size of the whole HID blob
                    // set.  A composite keyboard/mouse can have several
                    // independent report descriptors; summing them here
                    // makes the left-side legacy check reject the first HID
                    // interface even though every typed blob is complete.
                    // Keep the first HID report length as the primary value;
                    // the per-interface lengths in ProfileBlobChunkHeader
                    // remain authoritative for the generic proxy.
                    if (passthroughProfile.reportDescriptorLength == 0)
                        passthroughProfile.reportDescriptorLength =
                            proxy.expectedLength;
                }
            }

            hidDescriptorCounter++;
        }

        break;
    }

    case USB_B_DESCRIPTOR_TYPE_CONFIGURATION:
    {
        // Log raw bytes for USB_CONFIGURATION_DESC
        logRawBytes("USB_CONFIGURATION_DESC", p, p[0]);
        ESP_LOGI("EspUsbHost::onConfig", "Descriptor Type: USB_CONFIGURATION_DESC");

        const usb_config_desc_t *config_desc = (const usb_config_desc_t *)p;

        configurationDescriptor.bLength = config_desc->bLength;
        configurationDescriptor.bDescriptorType = config_desc->bDescriptorType;
        configurationDescriptor.wTotalLength = config_desc->wTotalLength;
        configurationDescriptor.bNumInterfaces = config_desc->bNumInterfaces;
        configurationDescriptor.bConfigurationValue = config_desc->bConfigurationValue;
        configurationDescriptor.iConfiguration = config_desc->iConfiguration;
        configurationDescriptor.bmAttributes = config_desc->bmAttributes;
        configurationDescriptor.bMaxPower = config_desc->bMaxPower;

        break;
    }

    default:
    {
        // Log raw bytes for unknown descriptor type
        logRawBytes("Unknown", p, p[0]);
        ESP_LOGI("EspUsbHost::onConfig", "Descriptor Type: Unknown (0x%02X)", bDescriptorType);
        break;
    }
    }
}


void EspUsbHost::_clientEventCallback(const usb_host_client_event_msg_t *eventMsg, void *arg)
{
    EspUsbHost *usbHost = static_cast<EspUsbHost *>(arg);
    esp_err_t err;

    switch (eventMsg->event)
    {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
    {
        if (usbHost->otaHostReserved.load(std::memory_order_acquire)) {
            usbHost->pendingDeviceAddress = eventMsg->new_dev.address;
            ESP_LOGI("EspUsbHost",
                     "Queued device address %u while OTA owns USB Host",
                     eventMsg->new_dev.address);
            return;
        }
        HostState expectedState = HostState::Empty;
        if (!usbHost->hostState.compare_exchange_strong(
                expectedState, HostState::Opening,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            if (expectedState == HostState::OtaReserved) {
                usbHost->pendingDeviceAddress = eventMsg->new_dev.address;
                ESP_LOGI("EspUsbHost",
                         "Queued device address %u for OTA-reserved host",
                         eventMsg->new_dev.address);
                return;
            }
        // A fast re-plug can queue NEW_DEV while the cleanup task is still
        // releasing the previous device. Never overwrite deviceHandle or the
        // transfer/interface arrays until that cleanup has completed.
        if (usbHost->hostState == HostState::Cleaning) {
            // Multi-mode controllers routinely disconnect and re-enumerate in
            // tens of milliseconds. Preserve the new address and finish
            // releasing the old handle; restarting the MCU here would force
            // such devices back into their transient boot persona forever.
            usbHost->pendingDeviceAddress = eventMsg->new_dev.address;
            ESP_LOGI("EspUsbHost",
                     "Queued device address %u while previous session cleans",
                     eventMsg->new_dev.address);
            return;
        }
        if (usbHost->hostState != HostState::Empty) {
            // Some host-controller event sequences report the replacement
            // address before the old DEV_GONE callback.  Never discard that
            // address: retire the old session and let the client task reopen
            // this queued device after teardown.
            usbHost->pendingDeviceAddress = eventMsg->new_dev.address;
            if (usbHost->hostState != HostState::Cleaning) {
                usbHost->hostState = HostState::Cleaning;
                usbHost->closingSessionId = usbHost->activeSessionId;
                usbHost->deviceGoneAccepted = false;
                usbHost->isReady = false;
                EspUsbHost::deviceConnected = false;
                deviceMouseReady = false;
                usbHost->streamStartPending = false;
                usbHost->lastStreamStartedMillis = 0;
                usbHost->streamStartRetries = 0;
                usbHost->resetInputQueues();
                usbHost->sendBridgeFrame(
                    makcm::bridge::MessageType::DeviceGone, 0, nullptr, 0,
                    usbHost->activeSessionId);
                usbHost->cleanupRequested = true;
            }
            ESP_LOGW("EspUsbHost", "Queued NEW_DEV address %u while state %u",
                     eventMsg->new_dev.address,
                     static_cast<unsigned>(usbHost->hostState.load(
                         std::memory_order_acquire)));
            return;
        }
        }

        // The address has now been consumed by the client task.  Do not let
        // a stale pending address trigger a duplicate open after a normal
        // NEW_DEV event wins the race with cleanup completion.
        usbHost->pendingDeviceAddress = 0;

        uint32_t session = ++usbHost->nextSessionId;
        if (session == 0) {
            session = ++usbHost->nextSessionId;
        }
        usbHost->activeSessionId = session;
        usbHost->closingSessionId = 0;
        usbHost->deviceGoneAccepted = false;
        ++usbHost->newDeviceEventCount;
        usbHost->serial1Send("ESPLOG_NEW_DEVICE:%u\n", eventMsg->new_dev.address);
        ESP_LOGI("EspUsbHost::_clientEventCallback", "Device connected");

        EspUsbHost::deviceConnected = false;
        usbHost->endpointCounter = 0;
        usbHost->interfaceCounter = 0;
        usbHost->hidDescriptorCounter = 0;
        usbHost->unknownDescriptorCounter = 0;
        usbHost->usbTransferSize = 0;
        usbHost->usbInterfaceSize = 0;
        usbHost->isDseEdgeDevice = false;
        usbHost->isCompx8kMouseDevice = false;
        usbHost->compxLastButtons = 0;
        deviceMouseReady = false;
        usbHost->streamStartPending = false;
        usbHost->lastStreamStartedMillis = 0;
        usbHost->streamStartRetries = 0;
        usbHost->currentInterfaceClaimed = false;
        usbHost->currentInterfaceNumber = 0xFF;
        usbHost->currentPhysicalInterfaceNumber = 0xFF;
        usbHost->selectedInterface = 0xFF;
        usbHost->selectedInEndpoint = 0;
        usbHost->selectedOutEndpoint = 0;
        usbHost->selectedInPacketSize = 0;
        usbHost->selectedOutPacketSize = 0;
        usbHost->auxiliaryMouseInterface = 0xFF;
        usbHost->auxiliaryMouseInEndpoint = 0;
        usbHost->auxiliaryMouseOutEndpoint = 0;
        usbHost->auxiliaryMouseInPacketSize = 0;
        usbHost->auxiliaryMouseOutPacketSize = 0;
        usbHost->passthroughProfile = {};
        usbHost->passthroughProfile.auxiliaryInterfaceNumber = 0xFF;
        usbHost->reportDescriptorLength = 0;
        usbHost->reportDescriptorReady = false;
        memset(usbHost->reportDescriptor, 0, sizeof(usbHost->reportDescriptor));
        usbHost->rawDeviceDescriptorLength = 0;
        usbHost->rawConfigurationDescriptorLength = 0;
        usbHost->rawConfigurationDeclaredLength = 0;
        usbHost->rawConfigurationInterfaceCount = 0;
        usbHost->rawConfigurationCrc16 = 0;
        usbHost->rawBosDescriptorLength = 0;
        usbHost->rawQualifierDescriptorLength = 0;
        usbHost->optionalDescriptorStage = 0;
        usbHost->rawTopologyRejectReason =
            makcm::bridge::CapabilityReason::None;
        usbHost->rawTopologySupported = true;
        usbHost->profileStableSinceMs = 0;
        usbHost->proxyHidDescriptorCount = 0;
        usbHost->nextProxyHidDescriptor = 0;
        usbHost->proxyStringDescriptorCount = 0;
        usbHost->nextProxyStringDescriptor = 0;
        usbHost->proxyStringLangid = 0x0409;
        usbHost->proxyLangIdCount = 0;
        usbHost->requiredStringIndexCount = 0;
        usbHost->proxyControlCacheCount = 0;
        usbHost->microsoftOsVendorCode = 0;
        usbHost->proxyEndpointCount = 0;
        usbHost->dualSenseControllerAdapter = false;
        usbHost->dualSenseTransform = {};
        usbHost->physicalConfigurationDescriptorLength = 0;
        memset(usbHost->rawDeviceDescriptor, 0,
               sizeof(usbHost->rawDeviceDescriptor));
        memset(usbHost->rawConfigurationDescriptor, 0,
               sizeof(usbHost->rawConfigurationDescriptor));
        memset(usbHost->rawBosDescriptor, 0,
               sizeof(usbHost->rawBosDescriptor));
        memset(usbHost->rawQualifierDescriptor, 0,
               sizeof(usbHost->rawQualifierDescriptor));
        memset(usbHost->proxyHidDescriptors, 0,
               sizeof(usbHost->proxyHidDescriptors));
        memset(usbHost->proxyStringDescriptors, 0,
               sizeof(usbHost->proxyStringDescriptors));
        memset(usbHost->proxyControlCache, 0,
               sizeof(usbHost->proxyControlCache));
        memset(usbHost->proxyEndpoints, 0, sizeof(usbHost->proxyEndpoints));
        memset(usbHost->endpoint_data_list, 0, sizeof(usbHost->endpoint_data_list));

        ESP_LOGD("EspUsbHost", "New device event detected. Raw event message:");

        usbHost->logRawBytes("New Device Event Message",
                             reinterpret_cast<const uint8_t *>(eventMsg),
                             sizeof(usb_host_client_event_msg_t));

        err = usb_host_device_open(usbHost->clientHandle, eventMsg->new_dev.address, &usbHost->deviceHandle);
        if (err != ESP_OK)
        {
            ESP_LOGE("EspUsbHost", "Failed to open device with address %d. Error: %d", eventMsg->new_dev.address, err);
            return;
        }

        ESP_LOGI("EspUsbHost", "Device opened successfully");

        usb_device_info_t dev_info;
        err = usb_host_device_info(usbHost->deviceHandle, &dev_info);
        if (err == ESP_OK)
        {
            ESP_LOGD("EspUsbHost", "Retrieved device info. Raw device info:");

            usbHost->logRawBytes("Device Info",
                                 reinterpret_cast<const uint8_t *>(&dev_info),
                                 sizeof(usb_device_info_t));

            usbHost->device_info.speed = dev_info.speed;
            usbHost->device_info.dev_addr = dev_info.dev_addr;
            usbHost->device_info.vMaxPacketSize0 = dev_info.bMaxPacketSize0;
            usbHost->device_info.bConfigurationValue = dev_info.bConfigurationValue;
            strcpy(usbHost->device_info.str_desc_manufacturer, getUsbDescString(dev_info.str_desc_manufacturer).c_str());
            strcpy(usbHost->device_info.str_desc_product, getUsbDescString(dev_info.str_desc_product).c_str());
            strcpy(usbHost->device_info.str_desc_serial_num, getUsbDescString(dev_info.str_desc_serial_num).c_str());

            ESP_LOGI("EspUsbHost", "Device info retrieved successfully");
        }
        else
        {
            ESP_LOGE("EspUsbHost", "Failed to retrieve device info. Error: %d", err);
        }

        const usb_device_desc_t *dev_desc;
        err = usb_host_get_device_descriptor(usbHost->deviceHandle, &dev_desc);
        if (err == ESP_OK)
        {
            ESP_LOGI("EspUsbHost::_clientEventCallback", "Device descriptor retrieved successfully");

            // Log raw bytes for the device descriptor
            usbHost->logRawBytes("Device Descriptor",
                                 reinterpret_cast<const uint8_t *>(dev_desc),
                                 sizeof(usb_device_desc_t));

            // Field-by-field assignment of the descriptor
            usbHost->descriptor_device.bLength = dev_desc->bLength;
            usbHost->descriptor_device.bDescriptorType = dev_desc->bDescriptorType;
            usbHost->descriptor_device.bcdUSB = dev_desc->bcdUSB;
            usbHost->descriptor_device.bDeviceClass = dev_desc->bDeviceClass;
            usbHost->descriptor_device.bDeviceSubClass = dev_desc->bDeviceSubClass;
            usbHost->descriptor_device.bDeviceProtocol = dev_desc->bDeviceProtocol;
            usbHost->descriptor_device.bMaxPacketSize0 = dev_desc->bMaxPacketSize0;
            usbHost->descriptor_device.idVendor = dev_desc->idVendor;
            usbHost->descriptor_device.idProduct = dev_desc->idProduct;
            usbHost->descriptor_device.bcdDevice = dev_desc->bcdDevice;
            usbHost->descriptor_device.iManufacturer = dev_desc->iManufacturer;
            usbHost->descriptor_device.iProduct = dev_desc->iProduct;
            usbHost->descriptor_device.iSerialNumber = dev_desc->iSerialNumber;
            usbHost->descriptor_device.bNumConfigurations = dev_desc->bNumConfigurations;
            usbHost->isDseEdgeDevice =
                dev_desc->idVendor == EspUsbHost::DSE_EDGE_VID &&
                dev_desc->idProduct == EspUsbHost::DSE_EDGE_PID;
            usbHost->isCompx8kMouseDevice =
                dev_desc->idVendor == EspUsbHost::COMPX_8K_VID &&
                dev_desc->idProduct == EspUsbHost::COMPX_8K_PID;
            memcpy(usbHost->rawDeviceDescriptor, dev_desc,
                   sizeof(usbHost->rawDeviceDescriptor));
            usbHost->rawDeviceDescriptorLength =
                sizeof(usbHost->rawDeviceDescriptor);
            ESP_LOGI("EspUsbHost", "Device VID:PID %04x:%04x, DSE Edge path: %s",
                     dev_desc->idVendor, dev_desc->idProduct,
                     usbHost->isDseEdgeDevice ? "yes" : "no");
        }
        else
        {
            ESP_LOGE("EspUsbHost", "Failed to retrieve device descriptor. Error: %d", err);
        }

        const usb_config_desc_t *config_desc;
        err = usb_host_get_active_config_descriptor(usbHost->deviceHandle, &config_desc);
        if (err == ESP_OK)
        {
            uint16_t declared_length = 0;
            uint8_t interface_count = 0;
            const uint16_t available_length = config_desc->wTotalLength;
            if (available_length >
                    sizeof(usbHost->rawConfigurationDescriptor) ||
                !validateConfigurationDescriptor(
                    reinterpret_cast<const uint8_t *>(config_desc),
                    available_length, declared_length, interface_count)) {
                usbHost->rawTopologySupported = false;
                usbHost->rawTopologyRejectReason =
                    makcm::bridge::CapabilityReason::DescriptorMalformed;
                usbHost->rawConfigurationDeclaredLength = declared_length;
                usbHost->rawConfigurationInterfaceCount = interface_count;
                ESP_LOGE("EspUsbHost",
                         "Configuration descriptor is invalid or too large: %u",
                         available_length);
                break;
            }

            // Keep the complete downstream blob for diagnostics and for the
            // physical endpoint/interface mapping.  The raw profile normally
            // mirrors this copy, except for the narrow DualSense controller
            // adapter below, which replaces it with a HID-only projection.
            memcpy(usbHost->physicalConfigurationDescriptor, config_desc,
                   declared_length);
            usbHost->physicalConfigurationDescriptorLength = declared_length;
            usbHost->dualSenseControllerAdapter = false;
            usbHost->dualSenseTransform = {};

            const uint8_t *profileConfiguration =
                usbHost->physicalConfigurationDescriptor;
            uint16_t profileConfigurationLength = declared_length;
            uint8_t profileInterfaceCount = interface_count;
            if (makcm::bridge::isDualSenseControllerDevice(
                    usbHost->rawDeviceDescriptor,
                    usbHost->rawDeviceDescriptorLength)) {
                makcm::bridge::DualSenseTransformResult transform;
                if (makcm::bridge::transformDualSenseConfiguration(
                        usbHost->physicalConfigurationDescriptor,
                        declared_length,
                        usbHost->rawConfigurationDescriptor,
                        sizeof(usbHost->rawConfigurationDescriptor),
                        transform)) {
                    usbHost->dualSenseControllerAdapter = true;
                    usbHost->dualSenseTransform = transform;
                    profileConfiguration = usbHost->rawConfigurationDescriptor;
                    profileConfigurationLength = transform.outputLength;
                    profileInterfaceCount = transform.interfaceCount;
                    ESP_LOGI("EspUsbHost",
                             "DualSense controller adapter retained %u HID interface(s), %u endpoint(s) from %u bytes",
                             transform.interfaceCount, transform.endpointCount,
                             declared_length);
                } else {
                    // Preserve the physical blob in the profile so the left
                    // side can return a deterministic capability reason.  Do
                    // not call _configCallback: claiming/arming audio or
                    // alternate-setting endpoints would violate the generic
                    // proxy limits after this adapter has rejected the blob.
                    memcpy(usbHost->rawConfigurationDescriptor,
                           usbHost->physicalConfigurationDescriptor,
                           declared_length);
                    usbHost->rawTopologySupported = false;
                    usbHost->rawTopologyRejectReason = transform.reason;
                    usbHost->reportDescriptorReady = true;
                    ESP_LOGE("EspUsbHost",
                             "DualSense controller adapter rejected configuration: reason=%u",
                             static_cast<unsigned>(transform.reason));
                }
            } else {
                memcpy(usbHost->rawConfigurationDescriptor,
                       usbHost->physicalConfigurationDescriptor,
                       declared_length);
            }
            usbHost->rawConfigurationDescriptorLength = profileConfigurationLength;
            usbHost->rawConfigurationDeclaredLength = profileConfigurationLength;
            usbHost->rawConfigurationInterfaceCount = profileInterfaceCount;
            usbHost->rawConfigurationCrc16 = makcm::bridge::crc16Ccitt(
                usbHost->rawConfigurationDescriptor,
                profileConfigurationLength);
            usbHost->passthroughProfile = {};
            usbHost->passthroughProfile.version =
                makcm::bridge::kProtocolVersion;
            usbHost->passthroughProfile.kind =
                makcm::bridge::DeviceKind::RawUsb;
            usbHost->passthroughProfile.auxiliaryInterfaceNumber = 0xFF;
            usbHost->passthroughProfile.deviceDescriptorLength =
                usbHost->rawDeviceDescriptorLength;
            usbHost->passthroughProfile.configurationDescriptorLength =
                usbHost->rawConfigurationDescriptorLength;
            // Log raw bytes for the configuration descriptor
            usbHost->logRawBytes("Configuration Descriptor",
                                 usbHost->rawConfigurationDescriptor,
                                 profileConfigurationLength);

            const usb_config_desc_t *profileConfig =
                reinterpret_cast<const usb_config_desc_t *>(
                    usbHost->rawConfigurationDescriptor);
            usbHost->configurationDescriptor.bLength = profileConfig->bLength;
            usbHost->configurationDescriptor.bDescriptorType = profileConfig->bDescriptorType;
            usbHost->configurationDescriptor.wTotalLength = profileConfig->wTotalLength;
            usbHost->configurationDescriptor.bNumInterfaces = profileConfig->bNumInterfaces;
            usbHost->configurationDescriptor.bConfigurationValue = profileConfig->bConfigurationValue;
            usbHost->configurationDescriptor.iConfiguration = profileConfig->iConfiguration;
            usbHost->configurationDescriptor.bmAttributes = profileConfig->bmAttributes;
            usbHost->configurationDescriptor.bMaxPower = profileConfig->bMaxPower;
            usbHost->descriptor_configuration.bLength = profileConfig->bLength;
            usbHost->descriptor_configuration.bDescriptorType = profileConfig->bDescriptorType;
            usbHost->descriptor_configuration.wTotalLength = profileConfig->wTotalLength;
            usbHost->descriptor_configuration.bNumInterfaces = profileConfig->bNumInterfaces;
            usbHost->descriptor_configuration.bConfigurationValue = profileConfig->bConfigurationValue;
            usbHost->descriptor_configuration.iConfiguration = profileConfig->iConfiguration;
            usbHost->descriptor_configuration.bmAttributes = profileConfig->bmAttributes;
            // Keep the raw descriptor unit (2 mA) in this observer copy;
            // DeviceProfile converts it once when publishing configPowerMa.
            usbHost->descriptor_configuration.bMaxPower = profileConfig->bMaxPower;

            ESP_LOGI("EspUsbHost::_clientEventCallback", "Configuration descriptor retrieved successfully");

            if (usbHost->dualSenseControllerAdapter) {
                usbHost->_configCallback(profileConfig);
            } else if (usbHost->rawTopologySupported) {
                usbHost->_configCallback(profileConfig);
            }
            EspUsbHost::deviceConnected = true;
            usbHost->hostState = HostState::Active;
            usbHost->profileStableSinceMs = millis();
        }
        else
        {
            ESP_LOGE("EspUsbHost", "Failed to retrieve configuration descriptor. Error: %d", err);
        }

        break;
    }

    case USB_HOST_CLIENT_EVENT_DEV_GONE:
    {
        if (usbHost->hostState == HostState::Empty ||
            usbHost->hostState == HostState::Cleaning) {
            break;
        }
        ESP_LOGI("EspUsbHost::_clientEventCallback", "Device disconnected");

        if (usbHost->isDseEdgeDevice) {
            usbHost->dseTriggerState.l2 = 0;
            usbHost->dseTriggerState.r2 = 0;
            usbHost->dseTriggerState.lastReportMillis = millis();
        }
        usbHost->resetInputQueues();
        usbHost->hostState = HostState::Cleaning;
        usbHost->closingSessionId = usbHost->activeSessionId;
        usbHost->deviceGoneAccepted = false;
        usbHost->isReady = false;
        EspUsbHost::deviceConnected = false;
        deviceMouseReady = false;
        usbHost->streamStartPending = false;
        usbHost->lastStreamStartedMillis = 0;
        usbHost->streamStartRetries = 0;
        usbHost->sendBridgeFrame(makcm::bridge::MessageType::DeviceGone,
                                 0, nullptr, 0, usbHost->activeSessionId);

        if (usbHost->cleanupComplete != nullptr) {
            xSemaphoreTake(usbHost->cleanupComplete, 0);
        }

        ESP_LOGI("EspUsbHost", "Notifying cleanup task...");
        // The USB client task owns the host-client context.  It will perform
        // teardown after this callback returns, then reopen any queued NEW_DEV
        // from that same task.
        usbHost->cleanupRequested = true;

        break;
    }

    default:
        break;
    }
}

void EspUsbHost::cleanupDevice()
{
    EspUsbHost *usbHost = this;

    ESP_LOGI("EspUsbHost", "Starting cleanup...");

    esp_err_t err;
    bool cleanupOk = true;

        // Retry the logical disconnect until the left acknowledges it. This
        // also gives NO_DEVICE transfer callbacks time to return without
        // resubmitting before their allocations are freed.
    for (int retry = 0; retry < 10 && !usbHost->deviceGoneAccepted; ++retry) {
        usbHost->sendBridgeFrame(makcm::bridge::MessageType::DeviceGone,
                                 retry + 1, nullptr, 0,
                                 usbHost->closingSessionId);
        vTaskDelay(pdMS_TO_TICKS(25));
    }

    for (int i = 0; i < usbHost->usbTransferSize; i++) {
        if (usbHost->usbTransfer[i] != NULL) {
            err = ESP_FAIL;
            for (int attempt = 0; attempt < 40; ++attempt) {
                err = usb_host_transfer_free(usbHost->usbTransfer[i]);
                if (err == ESP_OK) {
                    break;
                }
                // Freeing an in-flight transfer requires the client task to
                // dispatch its completion callback.  Keep servicing that
                // task context between retry attempts instead of sleeping in
                // a separate teardown task.
                usb_host_client_handle_events(usbHost->clientHandle,
                                              pdMS_TO_TICKS(5));
            }
            if (err == ESP_OK) {
                ESP_LOGI("EspUsbHost", "Freed USB transfer at index %d", i);
                usbHost->usbTransfer[i] = NULL;
            } else {
                cleanupOk = false;
                ESP_LOGE("EspUsbHost", "Failed to free USB transfer at index %d. Error: %d", i, err);
            }
        }
    }
    usbHost->usbTransferSize = 0;

    for (int i = 0; i < usbHost->usbInterfaceSize; i++) {
        err = usb_host_interface_release(usbHost->clientHandle,
                                          usbHost->deviceHandle,
                                          usbHost->usbInterface[i]);
        if (err == ESP_OK) {
            ESP_LOGI("EspUsbHost", "Released USB interface at index %d", i);
            usbHost->usbInterface[i] = 0;
        } else {
            cleanupOk = false;
            ESP_LOGE("EspUsbHost", "Failed to release USB interface at index %d. Error: %d", i, err);
        }
    }
    usbHost->usbInterfaceSize = 0;

    err = usbHost->deviceHandle != nullptr
        ? usb_host_device_close(usbHost->clientHandle, usbHost->deviceHandle)
        : ESP_OK;
    if (err == ESP_OK) {
        ESP_LOGI("EspUsbHost", "Device closed successfully");
    } else {
        cleanupOk = false;
        ESP_LOGE("EspUsbHost", "Failed to close device. Error: %d", err);
    }

    usbHost->deviceHandle = NULL;
    usbHost->isReady = false;
    usbHost->isDseEdgeDevice = false;
    usbHost->isCompx8kMouseDevice = false;
    usbHost->compxLastButtons = 0;
    usbHost->currentInterfaceClaimed = false;
    usbHost->currentInterfaceIsAuxMouse = false;
    usbHost->currentInterfaceNumber = 0xFF;
    usbHost->currentPhysicalInterfaceNumber = 0xFF;
    usbHost->dualSenseControllerAdapter = false;
    usbHost->dualSenseTransform = {};
    usbHost->physicalConfigurationDescriptorLength = 0;

    if (!cleanupOk) {
        ++usbHost->cleanupErrorCount;
        usbHost->hostState = HostState::Fault;
        // Do not restart from the USB client task before the UART RX task can
        // report OTA_UNAVAILABLE.  A restart here used to strand the Linux
        // updater with a silent timeout.  The host remains fail-closed until
        // a later physical re-enumeration/reboot, while the pending bootstrap
        // state machine gets a deterministic failure line.
        ESP_LOGE("EspUsbHost", "Hot-plug cleanup failed; host is fail-closed");
        if (usbHost->cleanupComplete != nullptr) {
            xSemaphoreGive(usbHost->cleanupComplete);
        }
        return;
    }

    usbHost->activeSessionId = 0;
    usbHost->streamStartPending = false;
    usbHost->lastStreamStartedMillis = 0;
    usbHost->streamStartRetries = 0;
    usbHost->profileStableSinceMs = 0;
    usbHost->reportDescriptorReady = false;
    usbHost->selectedInterface = 0xFF;
    usbHost->selectedInEndpoint = 0;
    usbHost->selectedOutEndpoint = 0;
    usbHost->auxiliaryMouseInterface = 0xFF;
    usbHost->auxiliaryMouseInEndpoint = 0;
    usbHost->auxiliaryMouseOutEndpoint = 0;
    ++usbHost->cleanupCount;
    usbHost->hostState = HostState::Empty;
    if (usbHost->cleanupComplete != nullptr) {
        xSemaphoreGive(usbHost->cleanupComplete);
    }

    ESP_LOGI("EspUsbHost", "Cleanup completed.");
    // If a fast re-plug queued NEW_DEV while the old device was Cleaning,
    // usbClientTask will consume pendingDeviceAddress on its next loop.
}


void EspUsbHost::_configCallback(const usb_config_desc_t *config_desc)
{
    const uint8_t *p = &config_desc->val[0];
    uint8_t bLength;

    ESP_LOGI("EspUsbHost", "Starting configuration descriptor processing");

    const uint8_t setup[8] = {
        0x80, 0x06, 0x00, 0x02, 0x00, 0x00, (uint8_t)config_desc->wTotalLength, 0x00};

    for (int i = 0; i < config_desc->wTotalLength; i += bLength, p += bLength)
    {
        bLength = *p;

        if (bLength < 2)
        {
            ESP_LOGE("EspUsbHost", "Invalid zero/short descriptor at offset %d", i);
            return;
        }

        if ((i + bLength) <= config_desc->wTotalLength)
        {
            const uint8_t bDescriptorType = *(p + 1);

            ESP_LOGD("EspUsbHost", "Processing descriptor of type: 0x%02X", bDescriptorType);

            this->onConfig(bDescriptorType, p);
        }
        else
        {
            ESP_LOGW("EspUsbHost", "Descriptor length exceeds total configuration length");
            return;
        }
    }

    ESP_LOGI("EspUsbHost", "Completed configuration descriptor processing");
    if (!rawTopologySupported) {
        ESP_LOGE("EspUsbHost", "USB topology is outside proxy limits");
        reportDescriptorReady = false;
    } else if (proxyHidDescriptorCount == 0) {
        prepareStringDescriptors();
    } else {
        nextProxyHidDescriptor = 0;
        requestNextHidDescriptor();
    }
}


void EspUsbHost::_onReceiveControl(usb_transfer_t *transfer)
{
    EspUsbHost *usbHost = static_cast<EspUsbHost *>(transfer->context);
    if (!usbHost)
    {
        return;
    }

     usbHost->logRawBytes("EspUsbHost::_onReceiveControl", transfer->data_buffer, transfer->actual_num_bytes);

    const int descriptorLength =
        static_cast<int>(transfer->actual_num_bytes) - 8;
    if (usbHost->nextProxyHidDescriptor >=
        usbHost->proxyHidDescriptorCount) {
        usbHost->rawTopologySupported = false;
        usbHost->rawTopologyRejectReason =
            makcm::bridge::CapabilityReason::BlobIncomplete;
    } else {
        ProxyHidDescriptor &descriptor =
            usbHost->proxyHidDescriptors[usbHost->nextProxyHidDescriptor];
        if (transfer->status == USB_TRANSFER_STATUS_COMPLETED &&
            descriptorLength == descriptor.expectedLength) {
            memcpy(descriptor.data, transfer->data_buffer + 8,
                   descriptorLength);
            descriptor.receivedLength = descriptorLength;
            for (int i = 0; i + 1 < descriptorLength; ++i) {
                if (descriptor.data[i] == 0x85) {
                    usbHost->passthroughProfile.flags |=
                        makcm::bridge::kProfileFlagReportIds;
                    break;
                }
            }
        } else {
            usbHost->rawTopologySupported = false;
            usbHost->rawTopologyRejectReason =
                makcm::bridge::CapabilityReason::BlobIncomplete;
            ESP_LOGE("EspUsbHost",
                     "HID report descriptor failed: slot=%u expected=%u actual=%d status=%u",
                     usbHost->nextProxyHidDescriptor,
                     descriptor.expectedLength, descriptorLength,
                     transfer->status);
        }
    }

    usb_host_transfer_free(transfer);
    ++usbHost->nextProxyHidDescriptor;
    usbHost->requestNextHidDescriptor();
}

void EspUsbHost::requestNextHidDescriptor()
{
    if (!rawTopologySupported) {
        reportDescriptorReady = false;
        return;
    }
    if (nextProxyHidDescriptor >= proxyHidDescriptorCount) {
        prepareStringDescriptors();
        return;
    }
    const ProxyHidDescriptor &descriptor =
        proxyHidDescriptors[nextProxyHidDescriptor];
    const uint8_t physicalInterface = proxyToPhysicalInterface(
        descriptor.interfaceNumber);
    if (physicalInterface == 0xFF ||
        submitControl(0x81, 0x00, 0x22, physicalInterface,
                      descriptor.expectedLength) != ESP_OK) {
        rawTopologySupported = false;
        rawTopologyRejectReason =
            makcm::bridge::CapabilityReason::BlobIncomplete;
        reportDescriptorReady = false;
    }
}

void EspUsbHost::prepareStringDescriptors()
{
    proxyStringDescriptorCount = 0;
    nextProxyStringDescriptor = 0;
    proxyStringLangid = 0x0409;
    proxyLangIdCount = 0;
    requiredStringIndexCount = 0;
    memset(proxyStringDescriptors, 0, sizeof(proxyStringDescriptors));
    memset(proxyLangIds, 0, sizeof(proxyLangIds));
    memset(requiredStringIndices, 0, sizeof(requiredStringIndices));

    // Collect the indexes first.  The LANGID descriptor has to be fetched
    // before we can request the same index in every language advertised by
    // the device, so the actual transfer queue is built in the callback.
    auto collectIndex = [this](uint8_t index) {
        if (index == 0 || index == kMicrosoftOsStringIndex) return;
        for (uint8_t i = 0; i < requiredStringIndexCount; ++i) {
            if (requiredStringIndices[i] == index) return;
        }
        if (requiredStringIndexCount >= sizeof(requiredStringIndices)) {
            rawTopologySupported = false;
            rawTopologyRejectReason =
                makcm::bridge::CapabilityReason::BlobIncomplete;
            return;
        }
        requiredStringIndices[requiredStringIndexCount++] = index;
    };

    if (rawDeviceDescriptorLength >= 18) {
        collectIndex(rawDeviceDescriptor[14]);
        collectIndex(rawDeviceDescriptor[15]);
        collectIndex(rawDeviceDescriptor[16]);
    }
    if (rawConfigurationDescriptorLength >= 9) {
        collectIndex(rawConfigurationDescriptor[6]);
        for (uint16_t offset = 0;
             offset < rawConfigurationDescriptorLength;) {
            const uint8_t length = rawConfigurationDescriptor[offset];
            if (length < 2 || offset + length >
                    rawConfigurationDescriptorLength) {
                rawTopologySupported = false;
                break;
            }
            const uint8_t type = rawConfigurationDescriptor[offset + 1];
            if (type == USB_B_DESCRIPTOR_TYPE_INTERFACE && length >= 9) {
                collectIndex(rawConfigurationDescriptor[offset + 8]);
            } else if (type == USB_B_DESCRIPTOR_TYPE_INTERFACE_ASSOCIATION &&
                       length >= 8) {
                collectIndex(rawConfigurationDescriptor[offset + 7]);
            }
            offset += length;
        }
    }
    if (!rawTopologySupported) {
        reportDescriptorReady = false;
        return;
    }

    if (requiredStringIndexCount == 0) {
        // A device without ordinary strings can still expose the optional
        // Microsoft OS string at index EE.  Treat it as a best-effort probe.
        proxyStringDescriptors[0].index = kMicrosoftOsStringIndex;
        proxyStringDescriptors[0].langid = 0;
        proxyStringDescriptorCount = 1;
    } else {
        proxyStringDescriptors[0].index = 0;
        proxyStringDescriptors[0].langid = 0;
        proxyStringDescriptorCount = 1;
    }
    requestNextStringDescriptor();
}

void EspUsbHost::buildStringDescriptorRequests()
{
    if (!rawTopologySupported || proxyStringDescriptorCount == 0) return;

    // Slot zero is the LANGID descriptor.  Preserve its captured bytes while
    // replacing the remainder with one request per (index, LANGID) pair.
    const bool hasLangIdDescriptor =
        proxyStringDescriptors[0].index == 0;
    proxyStringDescriptorCount = hasLangIdDescriptor ? 1 : 0;
    if (proxyLangIdCount == 0) {
        proxyLangIds[0] = 0x0409;
        proxyLangIdCount = 1;
    }
    proxyStringLangid = proxyLangIds[0];
    for (uint8_t i = 0; i < proxyLangIdCount; ++i) {
        if (proxyLangIds[i] == 0x0409) {
            proxyStringLangid = 0x0409;
            break;
        }
    }
    auto append = [this](uint8_t index, uint16_t langid) {
        for (uint8_t i = 0; i < proxyStringDescriptorCount; ++i) {
            if (proxyStringDescriptors[i].index == index &&
                proxyStringDescriptors[i].langid == langid) return true;
        }
        if (proxyStringDescriptorCount >=
            makcm::bridge::kMaxStringDescriptors) {
            rawTopologySupported = false;
            rawTopologyRejectReason =
                makcm::bridge::CapabilityReason::BlobIncomplete;
            return false;
        }
        ProxyStringDescriptor &slot =
            proxyStringDescriptors[proxyStringDescriptorCount++];
        slot = {};
        slot.index = index;
        slot.langid = langid;
        return true;
    };
    for (uint8_t lang = 0; lang < proxyLangIdCount; ++lang) {
        for (uint8_t index = 0; index < requiredStringIndexCount; ++index) {
            if (!append(requiredStringIndices[index], proxyLangIds[lang]))
                return;
        }
    }
    // Microsoft OS string index EE is requested with wIndex=0 and is
    // optional; a STALL is handled by _onStringDescriptor.
    append(kMicrosoftOsStringIndex, 0);
}

bool EspUsbHost::submitStringDescriptor(uint8_t index, uint16_t langid)
{
    usb_transfer_t *transfer = nullptr;
    constexpr uint16_t maxLength =
        makcm::bridge::kMaxStringDescriptorSize;
    if (deviceHandle == nullptr ||
        usb_host_transfer_alloc(8 + maxLength, 0, &transfer) != ESP_OK) {
        return false;
    }
    uint8_t *setup = transfer->data_buffer;
    setup[0] = 0x80;
    setup[1] = USB_B_REQUEST_GET_DESCRIPTOR;
    setup[2] = index;
    setup[3] = USB_B_DESCRIPTOR_TYPE_STRING;
    setup[4] = static_cast<uint8_t>(langid);
    setup[5] = static_cast<uint8_t>(langid >> 8);
    setup[6] = static_cast<uint8_t>(maxLength);
    setup[7] = static_cast<uint8_t>(maxLength >> 8);
    transfer->device_handle = deviceHandle;
    transfer->bEndpointAddress = 0;
    transfer->num_bytes = 8 + maxLength;
    transfer->callback = _onStringDescriptor;
    transfer->context = this;
    if (usb_host_transfer_submit_control(clientHandle, transfer) != ESP_OK) {
        usb_host_transfer_free(transfer);
        return false;
    }
    return true;
}

void EspUsbHost::requestNextStringDescriptor()
{
    if (!rawTopologySupported) {
        reportDescriptorReady = false;
        return;
    }
    if (nextProxyStringDescriptor >= proxyStringDescriptorCount) {
        requestMicrosoftOsDescriptors();
        return;
    }
    ProxyStringDescriptor &slot =
        proxyStringDescriptors[nextProxyStringDescriptor];
    if (!submitStringDescriptor(slot.index, slot.langid)) {
        rawTopologySupported = false;
        rawTopologyRejectReason =
            makcm::bridge::CapabilityReason::BlobIncomplete;
        reportDescriptorReady = false;
    }
}

void EspUsbHost::requestMicrosoftOsDescriptors()
{
    proxyControlCacheCount = 0;
    if (!rawTopologySupported || microsoftOsVendorCode == 0) {
        requestOptionalDescriptors();
        return;
    }
    // Microsoft OS 1.0 Extended Compat ID descriptor.  The XUSB binding
    // consumes this during enumeration; cache it with the profile so the
    // device side can answer locally before any stream traffic starts.
    ProxyControlCache &slot = proxyControlCache[proxyControlCacheCount];
    slot = {};
    slot.bmRequestType = 0xC0;
    slot.bRequest = microsoftOsVendorCode;
    slot.wValue = 0;
    slot.wIndex = 0x0004;
    slot.wLength = 0x0028;
    if (!submitMicrosoftOsDescriptor(proxyControlCacheCount)) {
        proxyControlCacheCount = 0;
        requestOptionalDescriptors();
        return;
    }
    ++proxyControlCacheCount;
}

bool EspUsbHost::submitMicrosoftOsDescriptor(uint8_t slotIndex)
{
    if (deviceHandle == nullptr || slotIndex >=
            makcm::bridge::kMaxCachedControls) return false;
    usb_transfer_t *transfer = nullptr;
    constexpr uint16_t length = 0x28;
    if (usb_host_transfer_alloc(8 + length, 0, &transfer) != ESP_OK)
        return false;
    ProxyOsControlContext *context = new (std::nothrow) ProxyOsControlContext;
    if (context == nullptr) {
        usb_host_transfer_free(transfer);
        return false;
    }
    context->host = this;
    context->slotIndex = slotIndex;
    uint8_t *setup = transfer->data_buffer;
    setup[0] = 0xC0;
    setup[1] = microsoftOsVendorCode;
    setup[2] = 0;
    setup[3] = 0;
    setup[4] = 0x04;
    setup[5] = 0x00;
    setup[6] = static_cast<uint8_t>(length);
    setup[7] = static_cast<uint8_t>(length >> 8);
    transfer->device_handle = deviceHandle;
    transfer->bEndpointAddress = 0;
    transfer->num_bytes = 8 + length;
    transfer->callback = _onMicrosoftOsDescriptor;
    transfer->context = context;
    if (usb_host_transfer_submit_control(clientHandle, transfer) != ESP_OK) {
        delete context;
        usb_host_transfer_free(transfer);
        return false;
    }
    return true;
}

void EspUsbHost::_onMicrosoftOsDescriptor(usb_transfer_t *transfer)
{
    ProxyOsControlContext *context =
        static_cast<ProxyOsControlContext *>(transfer->context);
    if (context == nullptr || context->host == nullptr) {
        usb_host_transfer_free(transfer);
        delete context;
        return;
    }
    EspUsbHost *host = context->host;
    const bool completed = transfer->status == USB_TRANSFER_STATUS_COMPLETED &&
                           transfer->actual_num_bytes >= 8;
    if (completed && context->slotIndex < host->proxyControlCacheCount) {
        ProxyControlCache &slot = host->proxyControlCache[context->slotIndex];
        const uint16_t length = std::min<uint16_t>(
            static_cast<uint16_t>(transfer->actual_num_bytes - 8),
            makcm::bridge::kMaxCachedControlSize);
        if (length != 0) {
            memcpy(slot.data, transfer->data_buffer + 8, length);
            slot.receivedLength = length;
            slot.wLength = length;
        }
    } else {
        host->proxyControlCacheCount = 0;
    }
    usb_host_transfer_free(transfer);
    delete context;
    host->requestOptionalDescriptors();
}

void EspUsbHost::requestOptionalDescriptors()
{
    if (!rawTopologySupported) {
        reportDescriptorReady = false;
        return;
    }
    if (optionalDescriptorStage == 0) {
        optionalDescriptorStage = 1;
        if (!submitOptionalDescriptor(
                makcm::bridge::ProfileBlobType::Bos)) {
            requestOptionalDescriptors();
        }
        return;
    }
    if (optionalDescriptorStage == 1) {
        optionalDescriptorStage = 2;
        if (!submitOptionalDescriptor(
                makcm::bridge::ProfileBlobType::DeviceQualifier)) {
            requestOptionalDescriptors();
        }
        return;
    }
    reportDescriptorReady = true;
}

bool EspUsbHost::submitOptionalDescriptor(
    makcm::bridge::ProfileBlobType type)
{
    const uint16_t maxLength = type == makcm::bridge::ProfileBlobType::Bos
                                   ? makcm::bridge::kMaxBosDescriptorSize
                                   : makcm::bridge::kMaxQualifierDescriptorSize;
    if (deviceHandle == nullptr) return false;
    usb_transfer_t *transfer = nullptr;
    if (usb_host_transfer_alloc(static_cast<uint16_t>(8 + maxLength), 0,
                                &transfer) != ESP_OK) {
        return false;
    }
    auto *context = new (std::nothrow) OptionalDescriptorContext;
    if (context == nullptr) {
        usb_host_transfer_free(transfer);
        return false;
    }
    context->host = this;
    context->type = type;
    uint8_t *setup = transfer->data_buffer;
    setup[0] = 0x80;
    setup[1] = USB_B_REQUEST_GET_DESCRIPTOR;
    setup[2] = 0;
    setup[3] = static_cast<uint8_t>(
        type == makcm::bridge::ProfileBlobType::Bos ? 0x0F : 0x06);
    setup[4] = 0;
    setup[5] = 0;
    setup[6] = static_cast<uint8_t>(maxLength);
    setup[7] = static_cast<uint8_t>(maxLength >> 8);
    transfer->device_handle = deviceHandle;
    transfer->bEndpointAddress = 0;
    transfer->num_bytes = static_cast<uint16_t>(8 + maxLength);
    transfer->callback = _onOptionalDescriptor;
    transfer->context = context;
    if (usb_host_transfer_submit_control(clientHandle, transfer) != ESP_OK) {
        delete context;
        usb_host_transfer_free(transfer);
        return false;
    }
    return true;
}

void EspUsbHost::_onOptionalDescriptor(usb_transfer_t *transfer)
{
    auto *context = static_cast<OptionalDescriptorContext *>(
        transfer != nullptr ? transfer->context : nullptr);
    EspUsbHost *host = context != nullptr ? context->host : nullptr;
    const uint16_t minimumDescriptorLength = context != nullptr &&
            context->type == makcm::bridge::ProfileBlobType::Bos ? 5 : 10;
    if (host != nullptr && transfer != nullptr &&
        transfer->status == USB_TRANSFER_STATUS_COMPLETED &&
        transfer->actual_num_bytes >= 8 + minimumDescriptorLength) {
        const uint16_t actual = static_cast<uint16_t>(
            transfer->actual_num_bytes - 8);
        const uint8_t *descriptor = transfer->data_buffer + 8;
        const uint8_t expectedType = context->type ==
            makcm::bridge::ProfileBlobType::Bos ? 0x0F : 0x06;
        uint16_t declared = descriptor[0];
        const uint16_t maxLength = context->type ==
            makcm::bridge::ProfileBlobType::Bos
                ? makcm::bridge::kMaxBosDescriptorSize
                : makcm::bridge::kMaxQualifierDescriptorSize;
        if (context->type == makcm::bridge::ProfileBlobType::Bos) {
            // BOS is a descriptor chain: bLength is only the header size;
            // wTotalLength covers every capability descriptor that must be
            // preserved in the raw Blob.
            if (actual >= 4) {
                declared = static_cast<uint16_t>(descriptor[2]) |
                           (static_cast<uint16_t>(descriptor[3]) << 8);
            } else {
                declared = 0;
            }
        }
        const bool validHeader = context->type ==
            makcm::bridge::ProfileBlobType::Bos
                ? descriptor[0] == 5 && declared >= 5
                : descriptor[0] == 10 && declared == 10;
        if (validHeader && declared <= actual && declared <= maxLength &&
            descriptor[1] == expectedType) {
            if (context->type == makcm::bridge::ProfileBlobType::Bos) {
                memcpy(host->rawBosDescriptor, descriptor, declared);
                host->rawBosDescriptorLength = declared;
            } else {
                memcpy(host->rawQualifierDescriptor, descriptor, declared);
                host->rawQualifierDescriptorLength = declared;
            }
        }
    }
    if (transfer != nullptr) usb_host_transfer_free(transfer);
    delete context;
    if (host != nullptr) host->requestOptionalDescriptors();
}

void EspUsbHost::_onStringDescriptor(usb_transfer_t *transfer)
{
    EspUsbHost *host = static_cast<EspUsbHost *>(transfer->context);
    if (host == nullptr) {
        usb_host_transfer_free(transfer);
        return;
    }
    bool valid = false;
    if (host->nextProxyStringDescriptor <
        host->proxyStringDescriptorCount &&
        transfer->status == USB_TRANSFER_STATUS_COMPLETED &&
        transfer->actual_num_bytes >= 10) {
        const uint16_t actual = transfer->actual_num_bytes - 8;
        const uint8_t *descriptor = transfer->data_buffer + 8;
        const uint8_t descriptorLength = descriptor[0];
        if (descriptor[1] == USB_B_DESCRIPTOR_TYPE_STRING &&
            descriptorLength >= 2 && descriptorLength <= actual &&
            descriptorLength <= makcm::bridge::kMaxStringDescriptorSize) {
            ProxyStringDescriptor &slot =
                host->proxyStringDescriptors[
                    host->nextProxyStringDescriptor];
            memcpy(slot.data, descriptor, descriptorLength);
            slot.receivedLength = descriptorLength;
            valid = true;
            if (slot.index == kMicrosoftOsStringIndex &&
                descriptorLength >= 17 && descriptor[2] == 'M' &&
                descriptor[4] == 'S' && descriptor[6] == 'F' &&
                descriptor[8] == 'T') {
                host->microsoftOsVendorCode = descriptor[16];
            }
            if (slot.index == 0 && descriptorLength >= 4) {
                host->proxyLangIdCount = 0;
                for (uint8_t offset = 2; offset + 1 < descriptorLength;
                     offset += 2) {
                    const uint16_t langid =
                        static_cast<uint16_t>(descriptor[offset]) |
                        (static_cast<uint16_t>(descriptor[offset + 1]) << 8);
                    bool duplicate = false;
                    for (uint8_t i = 0; i < host->proxyLangIdCount; ++i) {
                        if (host->proxyLangIds[i] == langid) {
                            duplicate = true;
                            break;
                        }
                    }
                    if (!duplicate && host->proxyLangIdCount <
                            sizeof(host->proxyLangIds) /
                                sizeof(host->proxyLangIds[0])) {
                        host->proxyLangIds[host->proxyLangIdCount++] = langid;
                    } else if (!duplicate) {
                        host->rawTopologySupported = false;
                        host->rawTopologyRejectReason =
                            makcm::bridge::CapabilityReason::BlobIncomplete;
                    }
                }
                host->buildStringDescriptorRequests();
            }
        }
    } else if (host->nextProxyStringDescriptor <
                   host->proxyStringDescriptorCount &&
               transfer->status == USB_TRANSFER_STATUS_STALL &&
               host->proxyStringDescriptors[
                   host->nextProxyStringDescriptor]
                       .index == kMicrosoftOsStringIndex) {
        // The OS string is optional.  Do not turn a normal STALL from a
        // device without Microsoft OS descriptors into profile rejection.
        valid = true;
    }
    usb_host_transfer_free(transfer);
    if (!valid) {
        host->rawTopologySupported = false;
        host->rawTopologyRejectReason =
            makcm::bridge::CapabilityReason::BlobIncomplete;
        host->reportDescriptorReady = false;
        return;
    }
    ++host->nextProxyStringDescriptor;
    host->requestNextStringDescriptor();
}


void EspUsbHost::onMouse(makcm::usb::BootMouseReport report,
                         uint8_t last_buttons)
{
    ESP_LOGI("EspUsbHost", "Mouse event detected");

    ESP_LOGD("EspUsbHost", 
             "Mouse State: last_buttons=0x%02x(%c%c%c%c%c), buttons=0x%02x(%c%c%c%c%c), x=%d, y=%d, wheel=%d",
             last_buttons,
             (last_buttons & makcm::usb::kMouseButtonLeft) ? 'L' : ' ',
             (last_buttons & makcm::usb::kMouseButtonRight) ? 'R' : ' ',
             (last_buttons & makcm::usb::kMouseButtonMiddle) ? 'M' : ' ',
             (last_buttons & makcm::usb::kMouseButtonBackward) ? 'B' : ' ',
             (last_buttons & makcm::usb::kMouseButtonForward) ? 'F' : ' ',
             report.buttons,
             (report.buttons & makcm::usb::kMouseButtonLeft) ? 'L' : ' ',
             (report.buttons & makcm::usb::kMouseButtonRight) ? 'R' : ' ',
             (report.buttons & makcm::usb::kMouseButtonMiddle) ? 'M' : ' ',
             (report.buttons & makcm::usb::kMouseButtonBackward) ? 'B' : ' ',
             (report.buttons & makcm::usb::kMouseButtonForward) ? 'F' : ' ',
             report.x,
             report.y,
             report.wheel);
}


void EspUsbHost::onMouseButtons(makcm::usb::BootMouseReport report,
                                uint8_t last_buttons)
{
    if (!(last_buttons & makcm::usb::kMouseButtonLeft) &&
        (report.buttons & makcm::usb::kMouseButtonLeft))
    {
        serial1Send("km.left(1)\n");
        ESP_LOGI("EspUsbHost", "Left mouse button pressed");
    }
    if ((last_buttons & makcm::usb::kMouseButtonLeft) &&
        !(report.buttons & makcm::usb::kMouseButtonLeft))
    {
        serial1Send("km.left(0)\n");
        ESP_LOGI("EspUsbHost", "Left mouse button released");
    }

    if (!(last_buttons & makcm::usb::kMouseButtonRight) &&
        (report.buttons & makcm::usb::kMouseButtonRight))
    {
        serial1Send("km.right(1)\n");
        ESP_LOGI("EspUsbHost", "Right mouse button pressed");
    }
    if ((last_buttons & makcm::usb::kMouseButtonRight) &&
        !(report.buttons & makcm::usb::kMouseButtonRight))
    {
        serial1Send("km.right(0)\n");
        ESP_LOGI("EspUsbHost", "Right mouse button released");
    }

    if (!(last_buttons & makcm::usb::kMouseButtonMiddle) &&
        (report.buttons & makcm::usb::kMouseButtonMiddle))
    {
        serial1Send("km.middle(1)\n");
        ESP_LOGI("EspUsbHost", "Middle mouse button pressed");
    }
    if ((last_buttons & makcm::usb::kMouseButtonMiddle) &&
        !(report.buttons & makcm::usb::kMouseButtonMiddle))
    {
        serial1Send("km.middle(0)\n");
        ESP_LOGI("EspUsbHost", "Middle mouse button released");
    }

    if (!(last_buttons & makcm::usb::kMouseButtonForward) &&
        (report.buttons & makcm::usb::kMouseButtonForward))
    {
        serial1Send("km.side1(1)\n");
        ESP_LOGI("EspUsbHost", "Forward mouse button pressed");
    }
    if ((last_buttons & makcm::usb::kMouseButtonForward) &&
        !(report.buttons & makcm::usb::kMouseButtonForward))
    {
        serial1Send("km.side1(0)\n");
        ESP_LOGI("EspUsbHost", "Forward mouse button released");
    }

    if (!(last_buttons & makcm::usb::kMouseButtonBackward) &&
        (report.buttons & makcm::usb::kMouseButtonBackward))
    {
        serial1Send("km.side2(1)\n");
        ESP_LOGI("EspUsbHost", "Backward mouse button pressed");
    }
    if ((last_buttons & makcm::usb::kMouseButtonBackward) &&
        !(report.buttons & makcm::usb::kMouseButtonBackward))
    {
        serial1Send("km.side2(0)\n");
        ESP_LOGI("EspUsbHost", "Backward mouse button released");
    }
}


void EspUsbHost::onMouseMove(makcm::usb::BootMouseReport report)
{
    if (report.wheel != 0)
    {
        serial1Send("km.wheel(%d)\n", report.wheel);
        ESP_LOGI("EspUsbHost", "Mouse wheel moved, value=%d", report.wheel);
    }
    else
    {
        serial1Send("km.move(%d,%d)\n", report.x, report.y);
        ESP_LOGI("EspUsbHost", "Mouse moved, x=%d, y=%d", report.x, report.y);
    }
}


void EspUsbHost::_onReceive(usb_transfer_t *transfer)
{
    EspUsbHost *usbHost = static_cast<EspUsbHost *>(transfer->context);
    if (!usbHost)
    {
        ESP_LOGE("EspUsbHost::_onReceive", "Error: Context pointer is null in _onReceive");
        usb_host_transfer_free(transfer);
        return;
    }

    if (usbHost->passthroughProfile.kind ==
        makcm::bridge::DeviceKind::RawUsb) {
        if (transfer->status == USB_TRANSFER_STATUS_COMPLETED &&
            transfer->actual_num_bytes > 0 &&
            transfer->actual_num_bytes <= 64 && deviceMouseReady) {
            uint8_t endpointFrame[66] = {};
            endpointFrame[0] = usbHost->physicalToProxyEndpoint(
                transfer->bEndpointAddress);
            if (endpointFrame[0] == 0) {
                // Filtered physical endpoints (DualSense audio) are never
                // exposed through the controller-only UART profile.
                if (usbHost->deviceHandle != nullptr &&
                    (usbHost->hostState == HostState::Opening ||
                     usbHost->hostState == HostState::Active))
                    usb_host_transfer_submit(transfer);
                return;
            }
            endpointFrame[1] = 0;
            memcpy(endpointFrame + 2, transfer->data_buffer,
                   transfer->actual_num_bytes);
            ++usbHost->inputReportCount;
            usbHost->queueInputReport(
                makcm::bridge::MessageType::EndpointInData, endpointFrame,
                static_cast<uint16_t>(transfer->actual_num_bytes + 2));
            usbHost->last_activity_time = millis();
            flashLED();
        }
        if (usbHost->deviceHandle != nullptr &&
            (usbHost->hostState == HostState::Opening ||
             usbHost->hostState == HostState::Active)) {
            const esp_err_t err = usb_host_transfer_submit(transfer);
            if (err != ESP_OK) {
                ESP_LOGE("EspUsbHost",
                         "Failed to resubmit proxy endpoint 0x%02x: 0x%x",
                         transfer->bEndpointAddress, err);
            }
        }
        return;
    }

    if (usbHost->passthroughProfile.kind == makcm::bridge::DeviceKind::XboxVendor &&
        usbHost->auxiliaryMouseInEndpoint != 0 &&
        transfer->bEndpointAddress == usbHost->auxiliaryMouseInEndpoint)
    {
        if (transfer->status == USB_TRANSFER_STATUS_COMPLETED &&
            transfer->actual_num_bytes > 0 && transfer->actual_num_bytes <= 64 &&
            deviceMouseReady) {
            ++usbHost->inputReportCount;
            uint8_t endpointFrame[66] = {};
            endpointFrame[0] = transfer->bEndpointAddress;
            endpointFrame[1] = 0;
            memcpy(endpointFrame + 2, transfer->data_buffer,
                   transfer->actual_num_bytes);
            usbHost->queueInputReport(
                makcm::bridge::MessageType::EndpointInData, endpointFrame,
                static_cast<uint16_t>(transfer->actual_num_bytes + 2));
            usbHost->last_activity_time = millis();
            flashLED();
        }
        if (EspUsbHost::deviceConnected &&
            usbHost->hostState == HostState::Active) {
            const esp_err_t err = usb_host_transfer_submit(transfer);
            if (err != ESP_OK) {
                ESP_LOGE("EspUsbHost", "Failed to resubmit Xbox mouse input: 0x%x", err);
            }
        }
        return;
    }

    if (usbHost->passthroughProfile.kind != makcm::bridge::DeviceKind::None &&
        transfer->bEndpointAddress == usbHost->selectedInEndpoint)
    {
        if (transfer->status == USB_TRANSFER_STATUS_COMPLETED &&
            transfer->actual_num_bytes > 0 && transfer->actual_num_bytes <= 64)
        {
            ++usbHost->inputReportCount;
            const uint8_t *raw = transfer->data_buffer;
            if (usbHost->isDseEdgeDevice && transfer->actual_num_bytes >= 7 && raw[0] == 0x01) {
                usbHost->dseTriggerState.l2 = raw[5];
                usbHost->dseTriggerState.r2 = raw[6];
                usbHost->dseTriggerState.lastReportMillis = millis();
                ++usbHost->dseTriggerState.reportCount;
            }
            usbHost->last_activity_time = millis();
            // Do not flood the shared UART before the left MCU has received
            // the device profile and attached its USB device stack. The left
            // sends ProfileAccepted only after that handshake is complete.
            if (deviceMouseReady) {
                uint8_t endpointFrame[66] = {};
                endpointFrame[0] = transfer->bEndpointAddress;
                endpointFrame[1] = 0;
                memcpy(endpointFrame + 2, raw,
                       transfer->actual_num_bytes);
                usbHost->queueInputReport(
                    makcm::bridge::MessageType::EndpointInData,
                    endpointFrame,
                    static_cast<uint16_t>(transfer->actual_num_bytes + 2));
            }
            flashLED();
        }
        if (EspUsbHost::deviceConnected &&
            usbHost->hostState == HostState::Active) {
            const esp_err_t err = usb_host_transfer_submit(transfer);
            if (err != ESP_OK) {
                ESP_LOGE("EspUsbHost", "Failed to resubmit passthrough input: 0x%x", err);
            }
        }
        return;
    }

    // Compx 373b:1278 interface 0 is a 7-byte Boot Mouse report with 16-bit
    // little-endian X/Y, followed by signed wheel and horizontal pan. The old
    // generic path truncated X/Y to 8 bits and also parsed vendor/keyboard
    // endpoints as mouse reports. Claiming interface 0 only plus this fixed
    // layout preserves the receiver's full movement range.
    if (usbHost->isCompx8kMouseDevice)
    {
        if (transfer->status == USB_TRANSFER_STATUS_COMPLETED &&
            transfer->bEndpointAddress == 0x81 && transfer->actual_num_bytes >= 7)
        {
            const uint8_t *raw = transfer->data_buffer;
            const uint8_t buttons = raw[0] & 0x1F;
            const int16_t x = static_cast<int16_t>(
                static_cast<uint16_t>(raw[1]) |
                (static_cast<uint16_t>(raw[2]) << 8));
            const int16_t y = static_cast<int16_t>(
                static_cast<uint16_t>(raw[3]) |
                (static_cast<uint16_t>(raw[4]) << 8));
            const int8_t wheel = static_cast<int8_t>(raw[5]);

            usbHost->last_activity_time = millis();
            flashLED();

            // The left MCU now enumerates its fixed DSE + mouse descriptor at
            // boot, so this path must not wait for the legacy descriptor-copy
            // profile acknowledgement to set deviceMouseReady.
            {
                if (buttons != usbHost->compxLastButtons)
                {
                    makcm::usb::BootMouseReport report = {};
                    report.buttons = buttons;
                    usbHost->onMouseButtons(report, usbHost->compxLastButtons);
                    usbHost->compxLastButtons = buttons;
                }
                if (x != 0 || y != 0)
                {
                    usbHost->serial1Send("km.move(%d,%d)\n", x, y);
                }
                if (wheel != 0)
                {
                    usbHost->serial1Send("km.wheel(%d)\n", wheel);
                }
            }
        }
        else if (transfer->status != USB_TRANSFER_STATUS_COMPLETED)
        {
            ESP_LOGW("EspUsbHost", "Compx input failed: status=0x%x endpoint=0x%x",
                     transfer->status, transfer->bEndpointAddress);
        }

        if (EspUsbHost::deviceConnected &&
            usbHost->hostState == HostState::Active)
        {
            const esp_err_t err = usb_host_transfer_submit(transfer);
            if (err != ESP_OK)
            {
                ESP_LOGE("EspUsbHost", "Failed to resubmit Compx input: err=0x%x", err);
            }
        }
        return;
    }

    // DSE Edge has a dedicated fast path. The callback only publishes the
    // trigger bytes and overwrites a one-element queue; UART framing/writes
    // happen in DseUartTx so USB polling is never held up by Serial1.
    if (usbHost->isDseEdgeDevice)
    {
        if (transfer->status == USB_TRANSFER_STATUS_COMPLETED &&
            transfer->actual_num_bytes == 64 &&
            transfer->data_buffer[0] == 0x01)
        {
            const uint8_t *raw = transfer->data_buffer;
            usbHost->dseTriggerState.l2 = raw[5];
            usbHost->dseTriggerState.r2 = raw[6];
            usbHost->dseTriggerState.lastReportMillis = millis();
            ++usbHost->dseTriggerState.reportCount;
            usbHost->last_activity_time = millis();
            uint8_t endpointFrame[66] = {};
            endpointFrame[0] = transfer->bEndpointAddress;
            endpointFrame[1] = 0;
            memcpy(endpointFrame + 2, raw, transfer->actual_num_bytes);
            usbHost->queueInputReport(
                makcm::bridge::MessageType::EndpointInData, endpointFrame,
                static_cast<uint16_t>(transfer->actual_num_bytes + 2));
            flashLED();
        }
        else if (transfer->status != USB_TRANSFER_STATUS_COMPLETED)
        {
            ESP_LOGW("EspUsbHost", "DSE input transfer failed: status=0x%x endpoint=0x%x",
                     transfer->status, transfer->bEndpointAddress);
        }

        if (EspUsbHost::deviceConnected &&
            usbHost->hostState == HostState::Active)
        {
            const esp_err_t err = usb_host_transfer_submit(transfer);
            if (err != ESP_OK)
            {
                ESP_LOGE("EspUsbHost", "Failed to resubmit DSE input: err=0x%x endpoint=0x%x",
                         err, transfer->bEndpointAddress);
            }
        }
        return;
    }

    static unsigned long lastLogTime = 0;
    unsigned long currentTime = millis();
    if (currentTime - lastLogTime >= 250)
    {
        usbHost->logRawBytes("EspUsbHost::_onReceive", transfer->data_buffer, transfer->actual_num_bytes);
        lastLogTime = currentTime;
    }

    uint8_t endpoint_num = transfer->bEndpointAddress & 0x0F;
    bool has_data = (transfer->actual_num_bytes > 0);

    if (has_data)
    {
        usbHost->last_activity_time = millis();
        flashLED();
    }

    ESP_LOGI("EspUsbHost", "Received HID report: %d bytes", transfer->actual_num_bytes);

     usbHost->logRawBytes("EspUsbHost::_onReceive HID Report", transfer->data_buffer, transfer->actual_num_bytes);

    // Process the HID report if it's a mouse report
    for (int i = 0; i < 16; i++)
    {
        if (usbHost->endpoint_data_list[i].bInterfaceClass == USB_CLASS_HID)
        {
            if (usbHost->endpoint_data_list[i].bInterfaceSubClass ==
                    makcm::usb::kHidBootSubclass &&
                usbHost->endpoint_data_list[i].bInterfaceProtocol ==
                    makcm::usb::kHidMouseProtocol)
            {
                static uint8_t last_buttons = 0;
                makcm::usb::BootMouseReport report = {};
                report.buttons = transfer->data_buffer[usbHost->HIDReportDesc.buttonStartByte];

                if (usbHost->HIDReportDesc.xAxisSize == 12 && usbHost->HIDReportDesc.yAxisSize == 12)
                {
                    uint8_t xyOffset = usbHost->HIDReportDesc.xAxisStartByte;
                    int16_t xValue = (transfer->data_buffer[xyOffset]) |
                                     ((transfer->data_buffer[xyOffset + 1] & 0x0F) << 8);
                    int16_t yValue = ((transfer->data_buffer[xyOffset + 1] >> 4) & 0x0F) |
                                     (transfer->data_buffer[xyOffset + 2] << 4);

                    report.x = xValue;
                    report.y = yValue;
                    uint8_t wheelOffset = usbHost->HIDReportDesc.wheelStartByte;
                    report.wheel = transfer->data_buffer[wheelOffset];
                }
                else
                {
                    uint8_t xOffset = usbHost->HIDReportDesc.xAxisStartByte;
                    uint8_t yOffset = usbHost->HIDReportDesc.yAxisStartByte;
                    uint8_t wheelOffset = usbHost->HIDReportDesc.wheelStartByte;

                    report.x = transfer->data_buffer[xOffset];
                    report.y = transfer->data_buffer[yOffset];
                    report.wheel = transfer->data_buffer[wheelOffset];
                }

                usbHost->onMouse(report, last_buttons);
                if (report.buttons != last_buttons)
                {
                    usbHost->onMouseButtons(report, last_buttons);
                    last_buttons = report.buttons;
                }
                if (report.x != 0 || report.y != 0 || report.wheel != 0)
                {
                    usbHost->onMouseMove(report);
                }
            }
        }
    }

    // Handle transfer status
    switch (transfer->status)
    {
    case USB_TRANSFER_STATUS_COMPLETED:
        ESP_LOGI("EspUsbHost", "Transfer completed successfully: Endpoint=0x%x", transfer->bEndpointAddress);
        break;

    case USB_TRANSFER_STATUS_STALL:
        ESP_LOGW("EspUsbHost", "Transfer STALL received: Endpoint=0x%x", transfer->bEndpointAddress);
        break;

    default:
        ESP_LOGE("EspUsbHost", "Transfer error or incomplete: Status=0x%x, Endpoint=0x%x", transfer->status, transfer->bEndpointAddress);
        break;
    }

    // This fallback path follows the same always-armed downstream policy.
    if (EspUsbHost::deviceConnected &&
        usbHost->hostState == HostState::Active)
    {
        esp_err_t err = usb_host_transfer_submit(transfer);
        if (err != ESP_OK)
        {
            ESP_LOGE("EspUsbHost", "Failed to resubmit transfer: err=0x%x, Endpoint=0x%x", err, transfer->bEndpointAddress);
        }
    }
}

esp_err_t EspUsbHost::submitControl(const uint8_t bmRequestType,
                                    const uint8_t bDescriptorIndex,
                                    const uint8_t bDescriptorType,
                                    const uint16_t wInterfaceNumber,
                                    const uint16_t wDescriptorLength)
{
    usb_transfer_t *transfer;
    esp_err_t err = usb_host_transfer_alloc(wDescriptorLength + 8 + 1, 0, &transfer);
    if (err != ESP_OK)
    {
        ESP_LOGE("EspUsbHost::submitControl", "usb_host_transfer_alloc() failed with err=0x%x", err);
        return err;
    }

    transfer->num_bytes = wDescriptorLength + 8;
    transfer->data_buffer[0] = bmRequestType;
    transfer->data_buffer[1] = 0x06;
    transfer->data_buffer[2] = bDescriptorIndex;
    transfer->data_buffer[3] = bDescriptorType;
    transfer->data_buffer[4] = wInterfaceNumber & 0xff;
    transfer->data_buffer[5] = wInterfaceNumber >> 8;
    transfer->data_buffer[6] = wDescriptorLength & 0xff;
    transfer->data_buffer[7] = wDescriptorLength >> 8;

    transfer->device_handle = deviceHandle;
    transfer->bEndpointAddress = 0x00;
    transfer->callback = _onReceiveControl;
    transfer->context = this;

    // Log raw bytes using the helper function
    logRawBytes("EspUsbHost::submitControl", transfer->data_buffer, transfer->num_bytes);

    ESP_LOGI("EspUsbHost", "Submitting control transfer, bmRequestType=0x%02x, bDescriptorIndex=0x%02x, bDescriptorType=0x%02x, wInterfaceNumber=0x%04x, wDescriptorLength=%d",
             bmRequestType, bDescriptorIndex, bDescriptorType, wInterfaceNumber, wDescriptorLength);

    err = usb_host_transfer_submit_control(clientHandle, transfer);
    if (err != ESP_OK)
    {
        ESP_LOGE("EspUsbHost", "usb_host_transfer_submit_control() failed with err=%x", err);
        usb_host_transfer_free(transfer);
    }
    else
    {
        ESP_LOGI("EspUsbHost", "Control transfer submitted successfully");
    }

    return err;
}

EspUsbHost::HIDReportDescriptor EspUsbHost::parseHIDReportDescriptor(uint8_t *data, int length)
{
    // Log the raw bytes using the helper function
    logRawBytes("EspUsbHost::parseHIDReportDescriptor", data, length);

    int i = 0;
    ParsedValues parsedValues = {0};

    auto getValue = [](uint8_t *data, int size, bool isSigned) -> int16_t
    {
        int16_t value = 0;
        if (isSigned)
        {
            if (size == 1)
            {
                value = (int8_t)data[0];
            }
            else if (size == 2)
            {
                value = (int16_t)(data[0] | (data[1] << 8));
            }
        }
        else
        {
            if (size == 1)
            {
                value = (uint8_t)data[0];
            }
            else if (size == 2)
            {
                value = (uint16_t)(data[0] | (data[1] << 8));
            }
        }
        ESP_LOGD("EspUsbHost::parseHIDReportDescriptor", "getValue: data[0]=0x%02X, size=%d, isSigned=%d, value=%d", data[0], size, isSigned, value);
        return value;
    };

    HIDReportDescriptor localHIDReportDesc = {0};

    ESP_LOGD("EspUsbHost::parseHIDReportDescriptor", "Starting parsing HID report descriptor of length %d", length);

    while (i < length)
    {
        uint8_t prefix = data[i];
        parsedValues.size = (prefix & 0x03);
        parsedValues.size = (parsedValues.size == 3) ? 4 : parsedValues.size;
        uint8_t item = prefix & 0xFC;
        bool isSigned = (item == 0x14 || item == 0x24);
        int16_t value = getValue(data + i + 1, parsedValues.size, isSigned);

        ESP_LOGD("EspUsbHost::parseHIDReportDescriptor", "At index %d: prefix=0x%02X, size=%d, item=0x%02X, isSigned=%d", i, prefix, parsedValues.size, item, isSigned);
        ESP_LOGD("EspUsbHost::parseHIDReportDescriptor", "Value extracted: %d", value);

        switch (item)
        {
        case 0x04: // USAGE_PAGE
            parsedValues.usagePage = (uint8_t)value;
            ESP_LOGD("EspUsbHost::parseHIDReportDescriptor", "USAGE_PAGE: %d", parsedValues.usagePage);
            break;
        case 0x08: // USAGE
            parsedValues.usage = (uint8_t)value;
            ESP_LOGD("EspUsbHost::parseHIDReportDescriptor", "USAGE: %d", parsedValues.usage);
            break;
        case 0x84: // REPORT_ID
            parsedValues.reportId = value;
            localHIDReportDesc.reportId = parsedValues.reportId;
            parsedValues.hasReportId = true;
            parsedValues.currentBitOffset += 8;
            ESP_LOGD("EspUsbHost::parseHIDReportDescriptor", "REPORT_ID: %d, currentBitOffset: %d", parsedValues.reportId, parsedValues.currentBitOffset);
            break;
        case 0x74: // REPORT_SIZE
            parsedValues.reportSize = value;
            ESP_LOGD("EspUsbHost::parseHIDReportDescriptor", "REPORT_SIZE: %d", parsedValues.reportSize);
            break;
        case 0x94: // REPORT_COUNT
            parsedValues.reportCount = value;
            ESP_LOGD("EspUsbHost::parseHIDReportDescriptor", "REPORT_COUNT: %d", parsedValues.reportCount);
            break;
        case 0x14: // LOGICAL_MINIMUM
            if (parsedValues.size == 1)
            {
                parsedValues.logicalMin8 = (int8_t)value;
                ESP_LOGD("EspUsbHost::parseHIDReportDescriptor", "LOGICAL_MINIMUM (8-bit): %d", parsedValues.logicalMin8);
            }
            else
            {
                parsedValues.logicalMin16 = value;
                ESP_LOGD("EspUsbHost::parseHIDReportDescriptor", "LOGICAL_MINIMUM (16-bit): %d", parsedValues.logicalMin16);
            }
            break;
        case 0x24: // LOGICAL_MAXIMUM
            if (parsedValues.size == 1)
            {
                parsedValues.logicalMax8 = (int8_t)value;
                ESP_LOGD("EspUsbHost::parseHIDReportDescriptor", "LOGICAL_MAXIMUM (8-bit): %d", parsedValues.logicalMax8);
            }
            else
            {
                parsedValues.logicalMax = value;
                ESP_LOGD("EspUsbHost::parseHIDReportDescriptor", "LOGICAL_MAXIMUM (16-bit): %d", parsedValues.logicalMax);
            }
            break;
        case 0xA0: // COLLECTION
            parsedValues.level++;
            parsedValues.collection = value;
            ESP_LOGD("EspUsbHost::parseHIDReportDescriptor", "COLLECTION: level=%d, collection=%d", parsedValues.level, parsedValues.collection);
            break;
        case 0xC0: // END_COLLECTION
            parsedValues.level--;
            ESP_LOGD("EspUsbHost::parseHIDReportDescriptor", "END_COLLECTION: level=%d", parsedValues.level);
            break;
        case 0x80: // INPUT
            ESP_LOGD("EspUsbHost::parseHIDReportDescriptor", "INPUT detected");
            ESP_LOGD("EspUsbHost::parseHIDReportDescriptor", "Current usagePage: %d, usage: %d", parsedValues.usagePage, parsedValues.usage);
            ESP_LOGD("EspUsbHost::parseHIDReportDescriptor", "Logical min: %d, Logical max: %d", parsedValues.logicalMin16, parsedValues.logicalMax);

            // Handle X and Y axis (Usage Page 0x01 and Usage 0x30 or 0x31)
            if (parsedValues.usagePage == 0x01 && (parsedValues.usage == 0x30 || parsedValues.usage == 0x31))
            {
                ESP_LOGD("EspUsbHost::parseHIDReportDescriptor", "Processing X/Y axis");
                if (parsedValues.logicalMax <= 2047)
                {
                    // Handle 12-bit range for X and Y axis
                    localHIDReportDesc.xAxisSize = 12;
                    localHIDReportDesc.xAxisStartByte = parsedValues.currentBitOffset / 8;
                    parsedValues.currentBitOffset += 12;
                    localHIDReportDesc.yAxisSize = 12;
                    localHIDReportDesc.yAxisStartByte = parsedValues.currentBitOffset / 8;
                    parsedValues.currentBitOffset += 12;
                    ESP_LOGD("EspUsbHost::parseHIDReportDescriptor", "X and Y axis (12-bit): xAxisSize=%d, yAxisSize=%d, xAxisStartByte=%d, yAxisStartByte=%d", localHIDReportDesc.xAxisSize, localHIDReportDesc.yAxisSize, localHIDReportDesc.xAxisStartByte, localHIDReportDesc.yAxisStartByte);
                }
                else
                {
                    // Handle 8-bit or 16-bit ranges
                    uint8_t axisSize = (parsedValues.logicalMax <= 127) ? 8 : 16;
                    localHIDReportDesc.xAxisSize = axisSize;
                    localHIDReportDesc.xAxisStartByte = parsedValues.currentBitOffset / 8;
                    parsedValues.currentBitOffset += axisSize;
                    localHIDReportDesc.yAxisSize = axisSize;
                    localHIDReportDesc.yAxisStartByte = parsedValues.currentBitOffset / 8;
                    parsedValues.currentBitOffset += axisSize;
                    ESP_LOGD("EspUsbHost::parseHIDReportDescriptor", "X and Y axis (%d-bit): xAxisSize=%d, yAxisSize=%d, xAxisStartByte=%d, yAxisStartByte=%d", axisSize, localHIDReportDesc.xAxisSize, localHIDReportDesc.yAxisSize, localHIDReportDesc.xAxisStartByte, localHIDReportDesc.yAxisStartByte);
                }
            }
            // Handle wheel movement (Usage Page 0x01 and Usage 0x38)
            else if (parsedValues.usagePage == 0x01 && parsedValues.usage == 0x38)
            {
                ESP_LOGD("EspUsbHost::parseHIDReportDescriptor", "Processing wheel movement");
                localHIDReportDesc.wheelSize = (parsedValues.logicalMax <= 127 && parsedValues.logicalMax >= -128) ? 8 : 16;
                localHIDReportDesc.wheelStartByte = parsedValues.currentBitOffset / 8;
                parsedValues.currentBitOffset += localHIDReportDesc.wheelSize;
                ESP_LOGD("EspUsbHost::parseHIDReportDescriptor", "Wheel movement: wheelSize=%d, wheelStartByte=%d", localHIDReportDesc.wheelSize, localHIDReportDesc.wheelStartByte);
            }
            // Handle buttons (Usage Page 0x09, Usage Minimum 0x01, Usage Maximum 0x10)
            else if (parsedValues.usagePage == 0x09 && parsedValues.usage >= 0x01 && parsedValues.usage <= 0x10)
            {
                ESP_LOGD("EspUsbHost::parseHIDReportDescriptor", "Processing buttons");
                localHIDReportDesc.buttonSize = parsedValues.reportCount * parsedValues.reportSize;
                localHIDReportDesc.buttonStartByte = parsedValues.currentBitOffset / 8;
                parsedValues.currentBitOffset += localHIDReportDesc.buttonSize;
                ESP_LOGD("EspUsbHost::parseHIDReportDescriptor", "Buttons: buttonSize=%d, buttonStartByte=%d, currentBitOffset=%d", localHIDReportDesc.buttonSize, localHIDReportDesc.buttonStartByte, parsedValues.currentBitOffset);
            }
            else
            {
                ESP_LOGD("EspUsbHost::parseHIDReportDescriptor", "Unhandled INPUT usage");
                parsedValues.currentBitOffset += parsedValues.reportSize * parsedValues.reportCount;
                ESP_LOGD("EspUsbHost::parseHIDReportDescriptor", "Updated currentBitOffset: %d", parsedValues.currentBitOffset);
            }
            break;
        default:
            ESP_LOGD("EspUsbHost::parseHIDReportDescriptor", "Unhandled item: 0x%02X", item);
            break;
        }

        i += parsedValues.size + 1;
        ESP_LOGD("EspUsbHost::parseHIDReportDescriptor", "Moving to next item, index now: %d", i);
    }

    HIDReportDesc = localHIDReportDesc;

    // Log final variable values
    ESP_LOGI("EspUsbHost::parseHIDReportDescriptor", "Final parsed values:");
    ESP_LOGI("EspUsbHost::parseHIDReportDescriptor", "reportId: %d", HIDReportDesc.reportId);
    ESP_LOGI("EspUsbHost::parseHIDReportDescriptor", "buttonSize: %d", HIDReportDesc.buttonSize);
    ESP_LOGI("EspUsbHost::parseHIDReportDescriptor", "xAxisSize: %d", HIDReportDesc.xAxisSize);
    ESP_LOGI("EspUsbHost::parseHIDReportDescriptor", "yAxisSize: %d", HIDReportDesc.yAxisSize);
    ESP_LOGI("EspUsbHost::parseHIDReportDescriptor", "wheelSize: %d", HIDReportDesc.wheelSize);
    ESP_LOGI("EspUsbHost::parseHIDReportDescriptor", "buttonStartByte: %d", HIDReportDesc.buttonStartByte);
    ESP_LOGI("EspUsbHost::parseHIDReportDescriptor", "xAxisStartByte: %d", HIDReportDesc.xAxisStartByte);
    ESP_LOGI("EspUsbHost::parseHIDReportDescriptor", "yAxisStartByte: %d", HIDReportDesc.yAxisStartByte);
    ESP_LOGI("EspUsbHost::parseHIDReportDescriptor", "wheelStartByte: %d", HIDReportDesc.wheelStartByte);

    return HIDReportDesc;
}
