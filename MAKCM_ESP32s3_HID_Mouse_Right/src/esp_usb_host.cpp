#include "EspUsbHost.h"
#include <sstream>
#include <iomanip>
#include "freertos/semphr.h"

#define USB_FEATURE_SELECTOR_REMOTE_WAKEUP 1

bool EspUsbHost::deviceMouseReady = false;
bool EspUsbHost::deviceConnected = false;
EspUsbHost::HIDReportDescriptor EspUsbHost::HIDReportDesc = {};
void flashLED();


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

            if (status & (1 << USB_FEATURE_SELECTOR_REMOTE_WAKEUP))
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

void EspUsbHost::suspend_device()
{
    const char *TAG = "suspend_device";

    usb_transfer_t *transfer;
    esp_err_t err = usb_host_transfer_alloc(8 + 1, 0, &transfer);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "usb_host_transfer_alloc() err=%X", err);
        return;
    }

    transfer->num_bytes = 8;
    transfer->data_buffer[0] = USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_STANDARD | USB_BM_REQUEST_TYPE_RECIP_DEVICE;
    transfer->data_buffer[1] = USB_B_REQUEST_SET_FEATURE;
    transfer->data_buffer[2] = USB_FEATURE_SELECTOR_REMOTE_WAKEUP;
    transfer->data_buffer[3] = 0x00;
    transfer->data_buffer[4] = 0x00;
    transfer->data_buffer[5] = 0x00;
    transfer->data_buffer[6] = 0x00;
    transfer->data_buffer[7] = 0x00;

    transfer->device_handle = deviceHandle;
    transfer->bEndpointAddress = 0x00;
    transfer->callback = _onReceiveControl;
    transfer->context = this;

    err = usb_host_transfer_submit_control(clientHandle, transfer);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "usb_host_transfer_submit_control() err=%X", err);
        usb_host_transfer_free(transfer);
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(50));

    deviceSuspended = true;

    ESP_LOGI(TAG, "Device suspended successfully.");

    get_device_status();
}


void EspUsbHost::resume_device()
{
    const char *TAG = "resume_device";

    if (!EspUsbHost::deviceConnected)
    {
        return;
    }

    usb_transfer_t *transfer;
    esp_err_t err = usb_host_transfer_alloc(8 + 1, 0, &transfer);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "usb_host_transfer_alloc() err=%X", err);
        return;
    }

    transfer->num_bytes = 8;
    transfer->data_buffer[0] = USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_STANDARD | USB_BM_REQUEST_TYPE_RECIP_DEVICE;
    transfer->data_buffer[1] = USB_B_REQUEST_CLEAR_FEATURE;
    transfer->data_buffer[2] = USB_FEATURE_SELECTOR_REMOTE_WAKEUP;
    transfer->data_buffer[3] = 0x00;
    transfer->data_buffer[4] = 0x00;
    transfer->data_buffer[5] = 0x00;
    transfer->data_buffer[6] = 0x00;
    transfer->data_buffer[7] = 0x00;

    transfer->device_handle = deviceHandle;
    transfer->bEndpointAddress = 0x00;
    transfer->callback = _onReceiveControl;
    transfer->context = this;

    err = usb_host_transfer_submit_control(clientHandle, transfer);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "usb_host_transfer_submit_control() err=%X", err);
        usb_host_transfer_free(transfer);
        return;
    }

    deviceSuspended = false;

    ESP_LOGI(TAG, "Device resumed successfully.");

    get_device_status();
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
    std::stringstream rawByteStream;
    
    for (int i = 0; i < length; ++i)
    {
        rawByteStream << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << (int)data[i] << " ";
    }

    ESP_LOGI(functionName, "Raw Bytes: %s", rawByteStream.str().c_str());
}

void EspUsbHost::onConfig(const uint8_t bDescriptorType, const uint8_t *p)
{
    static uint8_t currentInterfaceNumber;

        logRawBytes("EspUsbHost::onConfig", p, p[0]);

    switch (bDescriptorType)
    {
    case USB_DEVICE_DESC:
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

    case USB_STRING_DESC:
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

    case USB_INTERFACE_DESC:
    {
        // Log raw bytes for USB_INTERFACE_DESC
        logRawBytes("USB_INTERFACE_DESC", p, p[0]);
        ESP_LOGI("EspUsbHost::onConfig", "Descriptor Type: USB_INTERFACE_DESC");

        const usb_intf_desc_t *intf = (const usb_intf_desc_t *)p;

        if (interfaceCounter < MAX_INTERFACE_DESCRIPTORS)
        {
            this->claim_err = usb_host_interface_claim(this->clientHandle, this->deviceHandle, intf->bInterfaceNumber, intf->bAlternateSetting);
            if (this->claim_err != ESP_OK)
            {
                ESP_LOGE("EspUsbHost", "usb_host_interface_claim() err=%x", this->claim_err);
            }
            else
            {
                ESP_LOGI("EspUsbHost", "usb_host_interface_claim() successful for interface %d", intf->bInterfaceNumber);
                this->usbInterface[this->usbInterfaceSize] = intf->bInterfaceNumber;
                this->usbInterfaceSize++;

                currentInterfaceNumber = intf->bInterfaceNumber;
                endpoint_data_list[currentInterfaceNumber].bInterfaceNumber = intf->bInterfaceNumber;
                endpoint_data_list[currentInterfaceNumber].bInterfaceClass = intf->bInterfaceClass;
                endpoint_data_list[currentInterfaceNumber].bInterfaceSubClass = intf->bInterfaceSubClass;
                endpoint_data_list[currentInterfaceNumber].bInterfaceProtocol = intf->bInterfaceProtocol;
                endpoint_data_list[currentInterfaceNumber].bCountryCode = 0; // FIX THIS !!
            }

            interfaceCounter++;
        }

        break;
    }

    case USB_ENDPOINT_DESC:
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

            if (this->claim_err != ESP_OK)
            {
                ESP_LOGW("EspUsbHost", "Skipping endpoint due to claim_err.");
                return;
            }

            uint8_t ep_num = USB_EP_DESC_GET_EP_NUM(ep_desc);
            endpoint_data_list[ep_num].bInterfaceNumber = endpoint_data_list[currentInterfaceNumber].bInterfaceNumber;
            endpoint_data_list[ep_num].bInterfaceClass = endpoint_data_list[currentInterfaceNumber].bInterfaceClass;
            endpoint_data_list[ep_num].bInterfaceSubClass = endpoint_data_list[currentInterfaceNumber].bInterfaceSubClass;
            endpoint_data_list[ep_num].bInterfaceProtocol = endpoint_data_list[currentInterfaceNumber].bInterfaceProtocol;
            endpoint_data_list[ep_num].bCountryCode = endpoint_data_list[currentInterfaceNumber].bCountryCode;

            if ((ep_desc->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) != USB_BM_ATTRIBUTES_XFER_INT)
            {
                ESP_LOGE("EspUsbHost", "Unsupported transfer type: bmAttributes=%x", ep_desc->bmAttributes);
                return;
            }

            if (ep_desc->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK)
            {
                esp_err_t err = usb_host_transfer_alloc(ep_desc->wMaxPacketSize + 1, 0, &this->usbTransfer[this->usbTransferSize]);
                if (err != ESP_OK)
                {
                    this->usbTransfer[this->usbTransferSize] = NULL;
                    ESP_LOGE("EspUsbHost", "usb_host_transfer_alloc() failed with err=%x", err);
                    return;
                }
                else
                {
                    ESP_LOGI("EspUsbHost", "usb_host_transfer_alloc() successful, data_buffer_size=%d", ep_desc->wMaxPacketSize + 1);
                }

                this->usbTransfer[this->usbTransferSize]->device_handle = this->deviceHandle;
                this->usbTransfer[this->usbTransferSize]->bEndpointAddress = ep_desc->bEndpointAddress;
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
                }
            }

            endpointCounter++;
        }

        break;
    }

    case USB_INTERFACE_ASSOC_DESC:
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

    case USB_HID_DESC:
    {
        // Log raw bytes for USB_HID_DESC
        logRawBytes("USB_HID_DESC", p, p[0]);
        ESP_LOGI("EspUsbHost::onConfig", "Descriptor Type: USB_HID_DESC");

        const tusb_hid_descriptor_hid_t *hid_desc = (const tusb_hid_descriptor_hid_t *)p;

        if (hidDescriptorCounter < MAX_HID_DESCRIPTORS)
        {
            hid_descriptors[hidDescriptorCounter].bLength = hid_desc->bLength;
            hid_descriptors[hidDescriptorCounter].bDescriptorType = hid_desc->bDescriptorType;
            hid_descriptors[hidDescriptorCounter].bcdHID = hid_desc->bcdHID;
            hid_descriptors[hidDescriptorCounter].bCountryCode = hid_desc->bCountryCode;
            hid_descriptors[hidDescriptorCounter].bNumDescriptors = hid_desc->bNumDescriptors;
            hid_descriptors[hidDescriptorCounter].bReportType = hid_desc->bReportType;
            hid_descriptors[hidDescriptorCounter].wReportLength = hid_desc->wReportLength;
            endpoint_data_list[currentInterfaceNumber].bCountryCode = hid_descriptors[hidDescriptorCounter].bCountryCode;

            submitControl(0x81, 0x00, 0x22, currentInterfaceNumber, hid_descriptors[hidDescriptorCounter].wReportLength);

            hidDescriptorCounter++;
        }

        break;
    }

    case USB_CONFIGURATION_DESC:
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

    // Helper function to log raw bytes using std::stringstream
    auto logRawBytes = [](const char* descriptorType, const uint8_t* data, uint8_t length) {
        std::stringstream rawByteStream;
        for (int i = 0; i < length; ++i) {
            rawByteStream << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << (int)data[i] << " ";
        }
        ESP_LOGI("EspUsbHost::_clientEventCallback", "Raw Bytes (%s): %s", descriptorType, rawByteStream.str().c_str());
    };

    switch (eventMsg->event)
    {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
    {
        ESP_LOGI("EspUsbHost::_clientEventCallback", "Device connected");

        EspUsbHost::deviceConnected = true;
        usbHost->endpointCounter = 0;
        usbHost->interfaceCounter = 0;
        usbHost->hidDescriptorCounter = 0;
        usbHost->unknownDescriptorCounter = 0;
        memset(usbHost->endpoint_data_list, 0, sizeof(usbHost->endpoint_data_list));

        ESP_LOGD("EspUsbHost", "New device event detected. Raw event message:");

        logRawBytes("New Device Event Message", (const uint8_t *)eventMsg, sizeof(usb_host_client_event_msg_t));

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

            logRawBytes("Device Info", (const uint8_t *)&dev_info, sizeof(usb_device_info_t));

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
            logRawBytes("Device Descriptor", (const uint8_t *)dev_desc, sizeof(usb_device_desc_t));

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
        }
        else
        {
            ESP_LOGE("EspUsbHost", "Failed to retrieve device descriptor. Error: %d", err);
        }

        const usb_config_desc_t *config_desc;
        err = usb_host_get_active_config_descriptor(usbHost->deviceHandle, &config_desc);
        if (err == ESP_OK)
        {
            // Log raw bytes for the configuration descriptor
            logRawBytes("Configuration Descriptor", (const uint8_t *)config_desc, config_desc->wTotalLength);

            usbHost->descriptor_configuration.bLength = config_desc->bLength;
            usbHost->descriptor_configuration.bDescriptorType = config_desc->bDescriptorType;
            usbHost->descriptor_configuration.wTotalLength = config_desc->wTotalLength;
            usbHost->descriptor_configuration.bNumInterfaces = config_desc->bNumInterfaces;
            usbHost->descriptor_configuration.bConfigurationValue = config_desc->bConfigurationValue;
            usbHost->descriptor_configuration.iConfiguration = config_desc->iConfiguration;
            usbHost->descriptor_configuration.bmAttributes = config_desc->bmAttributes;
            usbHost->descriptor_configuration.bMaxPower = config_desc->bMaxPower * 2;

            ESP_LOGI("EspUsbHost::_clientEventCallback", "Configuration descriptor retrieved successfully");

            usbHost->_configCallback(config_desc);
        }
        else
        {
            ESP_LOGE("EspUsbHost", "Failed to retrieve configuration descriptor. Error: %d", err);
        }

        break;
    }

    case USB_HOST_CLIENT_EVENT_DEV_GONE:
    {
        ESP_LOGI("EspUsbHost::_clientEventCallback", "Device disconnected");

        usbHost->isReady = false;
        EspUsbHost::deviceConnected = false;
        deviceMouseReady = false;

        ESP_LOGI("EspUsbHost", "Notifying cleanup task...");
        xTaskNotifyGive(usbHost->cleanupTaskHandle); // Notify the cleanup task to start the process

        break;
    }

    default:
        break;
    }
}

void EspUsbHost::cleanupTask(void *arg)
{
    EspUsbHost *usbHost = static_cast<EspUsbHost *>(arg);

    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY); 

        ESP_LOGI("EspUsbHost", "Starting cleanup...");

        esp_err_t err;

        for (int i = 0; i < usbHost->usbTransferSize; i++) {
            if (usbHost->usbTransfer[i] != NULL) {
                err = usb_host_transfer_free(usbHost->usbTransfer[i]);
                if (err == ESP_OK) {
                    ESP_LOGI("EspUsbHost", "Freed USB transfer at index %d", i);
                    usbHost->usbTransfer[i] = NULL;
                } else {
                    ESP_LOGE("EspUsbHost", "Failed to free USB transfer at index %d. Error: %d", i, err);
                }
            }
        }
        usbHost->usbTransferSize = 0;

        for (int i = 0; i < usbHost->usbInterfaceSize; i++) {
            err = usb_host_interface_release(usbHost->clientHandle, usbHost->deviceHandle, usbHost->usbInterface[i]);
            if (err == ESP_OK) {
                ESP_LOGI("EspUsbHost", "Released USB interface at index %d", i);
                usbHost->usbInterface[i] = 0;
            } else {
                ESP_LOGE("EspUsbHost", "Failed to release USB interface at index %d. Error: %d", i, err);
            }
        }
        usbHost->usbInterfaceSize = 0;

        err = usb_host_device_close(usbHost->clientHandle, usbHost->deviceHandle);
        if (err == ESP_OK) {
            ESP_LOGI("EspUsbHost", "Device closed successfully");
        } else {
            ESP_LOGE("EspUsbHost", "Failed to close device. Error: %d", err);
        }

        usbHost->deviceHandle = NULL;
        usbHost->isReady = false;

        ESP_LOGI("EspUsbHost", "Cleanup completed.");
    }
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
}


void EspUsbHost::_onReceiveControl(usb_transfer_t *transfer)
{
    EspUsbHost *usbHost = static_cast<EspUsbHost *>(transfer->context);
    if (!usbHost)
    {
        return;
    }

     usbHost->logRawBytes("EspUsbHost::_onReceiveControl", transfer->data_buffer, transfer->actual_num_bytes);

    bool isMouse = false;
    uint8_t *p = &transfer->data_buffer[8];  // Skip the first 8 bytes for processing
    int totalBytes = transfer->actual_num_bytes;

    ESP_LOGI("EspUsbHost", "onReceiveControl called with %d bytes", totalBytes);

    // Check if it's a mouse
    for (int i = 0; i < totalBytes - 3; i++)
    {
        if (p[i] == 0x05 && p[i + 1] == 0x01 && p[i + 2] == 0x09 && p[i + 3] == 0x02)
        {
            isMouse = true;
            break;
        }
    }

    if (!isMouse)
    {
        ESP_LOGI("EspUsbHost", "Device is not a mouse, skipping further processing");
        usb_host_transfer_free(transfer);
        return;
    }

    ESP_LOGI("EspUsbHost", "Mouse device detected, parsing HID report descriptor");

    HIDReportDescriptor descriptor = usbHost->parseHIDReportDescriptor(&transfer->data_buffer[8], transfer->actual_num_bytes - 8);

    usb_host_transfer_free(transfer);
}


void EspUsbHost::onMouse(hid_mouse_report_t report, uint8_t last_buttons)
{
    ESP_LOGI("EspUsbHost", "Mouse event detected");

    ESP_LOGD("EspUsbHost", 
             "Mouse State: last_buttons=0x%02x(%c%c%c%c%c), buttons=0x%02x(%c%c%c%c%c), x=%d, y=%d, wheel=%d",
             last_buttons,
             (last_buttons & MOUSE_BUTTON_LEFT) ? 'L' : ' ',
             (last_buttons & MOUSE_BUTTON_RIGHT) ? 'R' : ' ',
             (last_buttons & MOUSE_BUTTON_MIDDLE) ? 'M' : ' ',
             (last_buttons & MOUSE_BUTTON_BACKWARD) ? 'B' : ' ',
             (last_buttons & MOUSE_BUTTON_FORWARD) ? 'F' : ' ',
             report.buttons,
             (report.buttons & MOUSE_BUTTON_LEFT) ? 'L' : ' ',
             (report.buttons & MOUSE_BUTTON_RIGHT) ? 'R' : ' ',
             (report.buttons & MOUSE_BUTTON_MIDDLE) ? 'M' : ' ',
             (report.buttons & MOUSE_BUTTON_BACKWARD) ? 'B' : ' ',
             (report.buttons & MOUSE_BUTTON_FORWARD) ? 'F' : ' ',
             report.x,
             report.y,
             report.wheel);
}


void EspUsbHost::onMouseButtons(hid_mouse_report_t report, uint8_t last_buttons)
{
    if (deviceMouseReady)
    {
        if (!(last_buttons & MOUSE_BUTTON_LEFT) && (report.buttons & MOUSE_BUTTON_LEFT))
        {
            serial1Send("km.left(1)\n");
            ESP_LOGI("EspUsbHost", "Left mouse button pressed");
        }
        if ((last_buttons & MOUSE_BUTTON_LEFT) && !(report.buttons & MOUSE_BUTTON_LEFT))
        {
            serial1Send("km.left(0)\n");
            ESP_LOGI("EspUsbHost", "Left mouse button released");
        }

        if (!(last_buttons & MOUSE_BUTTON_RIGHT) && (report.buttons & MOUSE_BUTTON_RIGHT))
        {
            serial1Send("km.right(1)\n");
            ESP_LOGI("EspUsbHost", "Right mouse button pressed");
        }
        if ((last_buttons & MOUSE_BUTTON_RIGHT) && !(report.buttons & MOUSE_BUTTON_RIGHT))
        {
            serial1Send("km.right(0)\n");
            ESP_LOGI("EspUsbHost", "Right mouse button released");
        }

        if (!(last_buttons & MOUSE_BUTTON_MIDDLE) && (report.buttons & MOUSE_BUTTON_MIDDLE))
        {
            serial1Send("km.middle(1)\n");
            ESP_LOGI("EspUsbHost", "Middle mouse button pressed");
        }
        if ((last_buttons & MOUSE_BUTTON_MIDDLE) && !(report.buttons & MOUSE_BUTTON_MIDDLE))
        {
            serial1Send("km.middle(0)\n");
            ESP_LOGI("EspUsbHost", "Middle mouse button released");
        }

        if (!(last_buttons & MOUSE_BUTTON_FORWARD) && (report.buttons & MOUSE_BUTTON_FORWARD))
        {
            serial1Send("km.side1(1)\n");
            ESP_LOGI("EspUsbHost", "Forward mouse button pressed");
        }
        if ((last_buttons & MOUSE_BUTTON_FORWARD) && !(report.buttons & MOUSE_BUTTON_FORWARD))
        {
            serial1Send("km.side1(0)\n");
            ESP_LOGI("EspUsbHost", "Forward mouse button released");
        }

        if (!(last_buttons & MOUSE_BUTTON_BACKWARD) && (report.buttons & MOUSE_BUTTON_BACKWARD))
        {
            serial1Send("km.side2(1)\n");
            ESP_LOGI("EspUsbHost", "Backward mouse button pressed");
        }
        if ((last_buttons & MOUSE_BUTTON_BACKWARD) && !(report.buttons & MOUSE_BUTTON_BACKWARD))
        {
            serial1Send("km.side2(0)\n");
            ESP_LOGI("EspUsbHost", "Backward mouse button released");
        }
    }
}


void EspUsbHost::onMouseMove(hid_mouse_report_t report)
{
    if (deviceMouseReady)
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
        if (EspUsbHost::deviceConnected && usbHost->deviceSuspended)
        {
            usbHost->resume_device();
        }
        flashLED();
    }

    ESP_LOGI("EspUsbHost", "Received HID report: %d bytes", transfer->actual_num_bytes);

     usbHost->logRawBytes("EspUsbHost::_onReceive HID Report", transfer->data_buffer, transfer->actual_num_bytes);

    // Process the HID report if it's a mouse report
    for (int i = 0; i < 16; i++)
    {
        if (usbHost->endpoint_data_list[i].bInterfaceClass == USB_CLASS_HID)
        {
            if (usbHost->endpoint_data_list[i].bInterfaceSubClass == HID_SUBCLASS_BOOT &&
                usbHost->endpoint_data_list[i].bInterfaceProtocol == HID_ITF_PROTOCOL_MOUSE)
            {
                static uint8_t last_buttons = 0;
                hid_mouse_report_t report = {};
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

    // Resubmit the transfer if the device is not suspended
    if (!usbHost->deviceSuspended)
    {
        esp_err_t err = usb_host_transfer_submit(transfer);
        if (err != ESP_OK)
        {
            ESP_LOGE("EspUsbHost", "Failed to resubmit transfer: err=0x%x, Endpoint=0x%x", err, transfer->bEndpointAddress);
        }
    }
    else
    {
        usb_host_transfer_free(transfer);
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
