#include "EspUsbHost.h"
#include "esp_task_wdt.h"
#include <driver/timer.h>
#include "freertos/queue.h"
#include "esp_log.h"

#define USB_TASK_STACK_SIZE 4096
#define USB_TASK_PRIORITY 1
#define CLIENT_TASK_PRIORITY 2
#define USB_FEATURE_SELECTOR_REMOTE_WAKEUP 1
bool EspUsbHost::deviceConnected = false;
bool EspUsbHost::deviceMouseReady = false;
EspUsbHost::HIDReportDescriptor EspUsbHost::HIDReportDesc;
bool enableYield = false;
const unsigned long ledFlashTime = 25; // Set The LED Flash timer in ms
void flashLED();

void EspUsbHost::begin(void)
{
  usbTransferSize = 0;
  deviceSuspended = false;
  last_activity_time = millis();

  const usb_host_config_t host_config = {
      .intr_flags = ESP_INTR_FLAG_LEVEL1,
  };

  esp_err_t err = usb_host_install(&host_config);
  Serial1.print("usb_host_install() status: ");
  Serial1.println(err);

  if (err != ESP_OK)
  {
    return;
  }

  txQueue = xQueueCreate(LOG_QUEUE_SIZE, LOG_MESSAGE_SIZE);
  xTaskCreate([](void *arg) { static_cast<EspUsbHost *>(arg)->esp_TX(arg); }, "esp_TX", 4096, this, 1, &espTxTaskHandle);
  xTaskCreate(esp_RX, "RxTask", 4048, this, 1, NULL);
  xTaskCreate(usb_lib_task, "usbLibTask", USB_TASK_STACK_SIZE, this, USB_TASK_PRIORITY, NULL);
  xTaskCreate(usb_client_task, "usbClientTask", USB_TASK_STACK_SIZE, this, CLIENT_TASK_PRIORITY, NULL);
  xTaskCreate(monitor_inactivity_task, "MonitorInactivityTask", 2048, this, 1, NULL);
}


void EspUsbHost::esp_RX(void *command)
{
  EspUsbHost *instance = static_cast<EspUsbHost *>(command);
  for (;;)
  {
    if (Serial1.available())
    {
      String command = Serial1.readStringUntil('\n');
      //Serial0.println(command);
      command.trim();
      instance->handleIncomingCommands(command);
      // Serial0.println("Received: " + command);
    }
    vTaskDelay(1);
  }
}

void EspUsbHost::esp_TX(void *arg)
{
    EspUsbHost *instance = static_cast<EspUsbHost *>(arg);
    char logMsg[LOG_MESSAGE_SIZE];
    uint32_t notificationValue;

    while (true)
    {
        xTaskNotifyWait(0, ULONG_MAX, &notificationValue, portMAX_DELAY);

        // Check if enableYield is true; if so, clear the queue and skip processing
        if (enableYield)
        {
            xQueueReset(instance->txQueue);
            //Serial1.println("Yield active");
            continue;
        }

        while (xQueueReceive(instance->txQueue, &logMsg, 0) == pdPASS)
        {
            Serial1.print(logMsg);
        }
    }
}


bool EspUsbHost::process_TX(const char *format, ...)
{
    static char logMsg[LOG_MESSAGE_SIZE];
    va_list args;
    va_start(args, format);
    vsnprintf(logMsg, sizeof(logMsg), format, args);
    va_end(args);

    // Check if enableYield is true; if so, clear the queue and don't add new messages
    if (enableYield)
    {
        xQueueReset(txQueue);
        //Serial1.println("TX blocked and queue cleared");
        return false;
    }

    if (xQueueSend(txQueue, logMsg, 0) == pdPASS)
    {
        
        xTaskNotifyGive(espTxTaskHandle);
    }
    else
    {
        Serial1.println("Queue Full");
        return false;
    }
    return true;
}


static void usb_lib_task(void *arg)
{
  EspUsbHost *instance = static_cast<EspUsbHost *>(arg);

  while (true)
  {
    uint32_t event_flags;
    esp_err_t err = usb_host_lib_handle_events(portMAX_DELAY, &event_flags);

    if (err != ESP_OK)
    {
      ESP_LOGI("EspUsbHost", "usb_host_lib_handle_events() err=%x", err);
      continue;
    }

    if (instance->clientHandle == NULL || !instance->isClientRegistering)
    {
      ESP_LOGI("EspUsbHost", "registering client...");
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
        ESP_LOGI("EspUsbHost", "Failed to re-register client, retrying...");
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

static void usb_client_task(void *arg)
{
  EspUsbHost *instance = static_cast<EspUsbHost *>(arg);

  while (true)
  {
    if (!instance->isClientRegistering)
    {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    esp_err_t err = usb_host_client_handle_events(instance->clientHandle, portMAX_DELAY);
  }
}

void EspUsbHost::monitor_inactivity_task(void *arg)
{
  const char *TAG = "monitor_inactivity_task";
  EspUsbHost *usbHost = (EspUsbHost *)arg;

  while (true)
  {
    vTaskDelay(pdMS_TO_TICKS(100));

    if (EspUsbHost::deviceConnected && !usbHost->deviceSuspended && millis() - usbHost->last_activity_time > 10000)
    {
      ESP_LOGI(TAG, "No activity for 10 seconds, suspending device");
      usbHost->suspend_device();
    }
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
    ESP_LOGI(TAG, "usb_host_transfer_alloc() err=%X", err);
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
    EspUsbHost *usbHost = (EspUsbHost *)transfer->context;

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
      ESP_LOGI(TAG, "GET_STATUS transfer failed with status=%X", transfer->status);
    }

    usb_host_transfer_free(transfer);
  };
  transfer->context = this;

  err = usb_host_transfer_submit_control(clientHandle, transfer);
  if (err != ESP_OK)
  {
    ESP_LOGI(TAG, "usb_host_transfer_submit_control() err=%X", err);
    usb_host_transfer_free(transfer);
    return;
  }
}

void EspUsbHost::suspend_device()
{
  const char *TAG = "suspend_device";
  usb_transfer_t *transfer;
  esp_err_t err = usb_host_transfer_alloc(8 + 1, 0, &transfer);
  if (err != ESP_OK)
  {
    ESP_LOGI(TAG, "usb_host_transfer_alloc() err=%X", err);
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
    ESP_LOGI(TAG, "usb_host_transfer_submit_control() err=%X", err);
  }

  vTaskDelay(pdMS_TO_TICKS(50));

  deviceSuspended = true;

  ESP_LOGI(TAG, "Device suspended successfully.");

  //Serial1.println("Device suspended successfully.");

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
    ESP_LOGI(TAG, "usb_host_transfer_alloc() err=%X", err);
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
    ESP_LOGI(TAG, "usb_host_transfer_submit_control() err=%X", err);
  }

  deviceSuspended = false;

  ESP_LOGI(TAG, "Device resumed successfully.");
  //Serial1.println("Device resumed successfully.");

  get_device_status();
}

void EspUsbHost::_printPcapText(const char *title, uint16_t function, uint8_t direction, uint8_t endpoint, uint8_t type, uint8_t size, uint8_t stage,
                                const uint8_t *data)
{
#if (ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO)
  uint8_t urbsize = 0x1c;
  if (stage == 0xff)
  {
    urbsize = 0x1b;
  }

  String data_str = "";
  for (int i = 0; i < size; i++)
  {
    if (data[i] < 16)
    {
      data_str += "0";
    }
    data_str += String(data[i], HEX) + " ";
  }

  printf("\n");
  printf("[PCAP TEXT]%s\n", title);
  printf("0000  %02x 00 00 00 00 00 00 00 00 00 00 00 00 00 %02x %02x\n", urbsize, (function & 0xff), ((function >> 8) & 0xff));
  printf("0010  %02x 01 00 01 00 %02x %02x %02x 00 00 00", direction, endpoint, type, size);
  if (stage != 0xff)
  {
    printf(" %02x\n", stage);
  }
  else
  {
    printf("\n");
  }
  printf("00%02x  %s\n", urbsize, data_str.c_str());
  printf("\n");
#endif
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

void EspUsbHost::onConfig(const uint8_t bDescriptorType, const uint8_t *p)
{
  static uint8_t currentInterfaceNumber;

  switch (bDescriptorType)
  {
  case USB_DEVICE_DESC:
  {
    const usb_device_desc_t *dev_desc = (const usb_device_desc_t *)p;
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
    const usb_intf_desc_t *intf = (const usb_intf_desc_t *)p;

    if (interfaceCounter < MAX_INTERFACE_DESCRIPTORS)
    {
      this->claim_err = usb_host_interface_claim(this->clientHandle, this->deviceHandle, intf->bInterfaceNumber, intf->bAlternateSetting);
      if (this->claim_err != ESP_OK)
      {
        ESP_LOGI("USB", "usb_host_interface_claim() err=%x", claim_err);
      }
      else
      {
        ESP_LOGI("USB", "usb_host_interface_claim() ESP_OK");
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
          (ep_desc->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) == USB_BM_ATTRIBUTES_XFER_CONTROL ? "CTRL" : (ep_desc->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) == USB_BM_ATTRIBUTES_XFER_ISOC ? "ISOC"
                                                                                                             : (ep_desc->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) == USB_BM_ATTRIBUTES_XFER_BULK   ? "BULK"
                                                                                                             : (ep_desc->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) == USB_BM_ATTRIBUTES_XFER_INT    ? "Interrupt"
                                                                                                                                                                                                          : "";
      endpoint_descriptors[endpointCounter].wMaxPacketSize = ep_desc->wMaxPacketSize;
      endpoint_descriptors[endpointCounter].bInterval = ep_desc->bInterval;

      if (this->claim_err != ESP_OK)
      {
        ESP_LOGI("USB", "claim_err skip");
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
        process_TX("err ep_desc->bmAttributes=%x", ep_desc->bmAttributes);
        return;
      }

      if (ep_desc->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK)
      {
        esp_err_t err = usb_host_transfer_alloc(ep_desc->wMaxPacketSize + 1, 0, &this->usbTransfer[this->usbTransferSize]);
        if (err != ESP_OK)
        {
          this->usbTransfer[this->usbTransferSize] = NULL;
          ESP_LOGI("USB", "usb_host_transfer_alloc() err=%x", err);
          return;
        }
        else
        {
          ESP_LOGI("USB", "usb_host_transfer_alloc() ESP_OK data_buffer_size=%d", ep_desc->wMaxPacketSize + 1);
        }

        this->usbTransfer[this->usbTransferSize]->device_handle = this->deviceHandle;
        this->usbTransfer[this->usbTransferSize]->bEndpointAddress = ep_desc->bEndpointAddress;
        this->usbTransfer[this->usbTransferSize]->callback = this->_onReceive;
        this->usbTransfer[this->usbTransferSize]->context = this;
        this->usbTransfer[this->usbTransferSize]->num_bytes = ep_desc->wMaxPacketSize;
        interval = ep_desc->bInterval;
        isReady = true;
        this->usbTransferSize++;

        ESP_LOGI("USB", "usb_host_transfer_submit for endpoint 0x%x", ep_desc->bEndpointAddress);

        err = usb_host_transfer_submit(this->usbTransfer[this->usbTransferSize - 1]);
        if (err != ESP_OK)
        {
          ESP_LOGI("USB", "usb_host_transfer_submit() err=%x", err);
        }
      }

      endpointCounter++;
    }

    break;
  }

  case USB_INTERFACE_ASSOC_DESC:
  {
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

  default:
  {
    const usb_standard_desc_t *desc = (const usb_standard_desc_t *)p;

    if (unknownDescriptorCounter < MAX_UNKNOWN_DESCRIPTORS)
    {
      unknown_descriptors[unknownDescriptorCounter].bLength = desc->bLength;
      unknown_descriptors[unknownDescriptorCounter].bDescriptorType = desc->bDescriptorType;
      unknown_descriptors[unknownDescriptorCounter].data = "";

      for (int i = 0; i < (desc->bLength - 2); i++)
      {
        if (desc->val[i] < 16)
        {
          unknown_descriptors[unknownDescriptorCounter].data += "0";
        }
        unknown_descriptors[unknownDescriptorCounter].data += String(desc->val[i], HEX) + " ";
      }

      unknownDescriptorCounter++;
    }
    else
    {
      // What if.
    }
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
    Serial1.println("DEVICE CONNECTED");
    EspUsbHost::deviceConnected = true;
    usbHost->endpointCounter = 0;
    usbHost->interfaceCounter = 0;
    usbHost->hidDescriptorCounter = 0;
    usbHost->unknownDescriptorCounter = 0;
    memset(usbHost->endpoint_data_list, 0, sizeof(usbHost->endpoint_data_list));

    err = usb_host_device_open(usbHost->clientHandle, eventMsg->new_dev.address, &usbHost->deviceHandle);
    if (err != ESP_OK)
      return;

    usb_device_info_t dev_info;
    err = usb_host_device_info(usbHost->deviceHandle, &dev_info);
    if (err == ESP_OK)
    {
      usbHost->device_info.speed = dev_info.speed;
      usbHost->device_info.dev_addr = dev_info.dev_addr;
      usbHost->device_info.vMaxPacketSize0 = dev_info.bMaxPacketSize0;
      usbHost->device_info.bConfigurationValue = dev_info.bConfigurationValue;
      strcpy(usbHost->device_info.str_desc_manufacturer, getUsbDescString(dev_info.str_desc_manufacturer).c_str());
      strcpy(usbHost->device_info.str_desc_product, getUsbDescString(dev_info.str_desc_product).c_str());
      strcpy(usbHost->device_info.str_desc_serial_num, getUsbDescString(dev_info.str_desc_serial_num).c_str());
    }

    const usb_device_desc_t *dev_desc;
    err = usb_host_get_device_descriptor(usbHost->deviceHandle, &dev_desc);
    if (err == ESP_OK)
    {
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

    const usb_config_desc_t *config_desc;
    err = usb_host_get_active_config_descriptor(usbHost->deviceHandle, &config_desc);
    if (err == ESP_OK)
    {
      usbHost->descriptor_configuration.bLength = config_desc->bLength;
      usbHost->descriptor_configuration.bDescriptorType = config_desc->bDescriptorType;
      usbHost->descriptor_configuration.wTotalLength = config_desc->wTotalLength;
      usbHost->descriptor_configuration.bNumInterfaces = config_desc->bNumInterfaces;
      usbHost->descriptor_configuration.bConfigurationValue = config_desc->bConfigurationValue;
      usbHost->descriptor_configuration.iConfiguration = config_desc->iConfiguration;
      usbHost->descriptor_configuration.bmAttributes = config_desc->bmAttributes;
      usbHost->descriptor_configuration.bMaxPower = config_desc->bMaxPower * 2;

      usbHost->_configCallback(config_desc);
    }
    break;
  }

  case USB_HOST_CLIENT_EVENT_DEV_GONE:
  {
    usbHost->isReady = false;
    EspUsbHost::deviceConnected = false;
    deviceMouseReady = false;

    for (int i = 0; i < usbHost->usbTransferSize; i++)
    {
      if (usbHost->usbTransfer[i] == NULL)
        continue;

      err = usb_host_transfer_free(usbHost->usbTransfer[i]);
      if (err == ESP_OK)
      {
        usbHost->usbTransfer[i] = NULL;
      }
    }
    usbHost->usbTransferSize = 0;

    for (int i = 0; i < usbHost->usbInterfaceSize; i++)
    {
      err = usb_host_interface_release(usbHost->clientHandle, usbHost->deviceHandle, usbHost->usbInterface[i]);
      if (err == ESP_OK)
      {
        usbHost->usbInterface[i] = 0;
      }
    }
    usbHost->usbInterfaceSize = 0;

    err = usb_host_device_close(usbHost->clientHandle, usbHost->deviceHandle);
    if (err != ESP_OK)
    {
      Serial1.println("Error closing device");
    }
    else
    {
      // Serial0.println("Device closed successfully");
    }

    usbHost->isReady = false;
    usbHost->isClientRegistering = false;

    // Notify the rest of the system
    usbHost->onGone(eventMsg);
    usbHost->process_TX("USB_GOODBYE\n");
    Serial1.println("MOUSE DISCONNECTED ");

    break;
  }
  default:
    break;
  }
}

void EspUsbHost::_configCallback(const usb_config_desc_t *config_desc)
{
  const uint8_t *p = &config_desc->val[0];
  uint8_t bLength;

  const uint8_t setup[8] = {
      0x80,
      0x06,
      0x00,
      0x02,
      0x00,
      0x00,
      (uint8_t)config_desc->wTotalLength,
      0x00};

  for (int i = 0; i < config_desc->wTotalLength; i += bLength, p += bLength)
  {
    bLength = *p;
    if ((i + bLength) <= config_desc->wTotalLength)
    {
      const uint8_t bDescriptorType = *(p + 1);
      this->onConfig(bDescriptorType, p);
    }
    else
    {
      return;
    }
  }
  // this->sendALLDescriptors();
}

void EspUsbHost::_onReceive(usb_transfer_t *transfer)
{
  EspUsbHost *usbHost = (EspUsbHost *)transfer->context;
  uint8_t endpoint_num = transfer->bEndpointAddress & 0x0F;

  bool has_data = (transfer->actual_num_bytes > 0);

  if (has_data)
  {
    usbHost->last_activity_time = millis();
    if (EspUsbHost::deviceConnected)
    {
      if (usbHost->deviceSuspended)
      {
        usbHost->resume_device();
      }
    }
     flashLED();
  }

  for (int i = 0; i < 16; i++)
  {
    if (usbHost->endpoint_data_list[i].bInterfaceClass == USB_CLASS_HID)
    {
      if (usbHost->endpoint_data_list[i].bInterfaceSubClass == HID_SUBCLASS_BOOT &&
          usbHost->endpoint_data_list[i].bInterfaceProtocol == HID_ITF_PROTOCOL_MOUSE)
      {
        static uint8_t last_buttons = 0;
        hid_mouse_report_t report = {};

        report.buttons = transfer->data_buffer[HIDReportDesc.buttonStartByte];

        if (HIDReportDesc.xAxisSize == 12 && HIDReportDesc.yAxisSize == 12)
        {
          uint8_t xyOffset = HIDReportDesc.xAxisStartByte;

          int16_t xValue = (transfer->data_buffer[xyOffset]) |
                           ((transfer->data_buffer[xyOffset + 1] & 0x0F) << 8);

          int16_t yValue = ((transfer->data_buffer[xyOffset + 1] >> 4) & 0x0F) |
                           (transfer->data_buffer[xyOffset + 2] << 4);

          report.x = xValue;
          report.y = yValue;

          uint8_t wheelOffset = HIDReportDesc.wheelStartByte;
          report.wheel = transfer->data_buffer[wheelOffset];
        }
        else
        {
          uint8_t xOffset = HIDReportDesc.xAxisStartByte;
          uint8_t yOffset = HIDReportDesc.yAxisStartByte;
          uint8_t wheelOffset = HIDReportDesc.wheelStartByte;

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

  if (transfer->status == USB_TRANSFER_STATUS_COMPLETED)
  {
    ESP_LOGI("EspUsbHost", "Transfer Completed Successfully: Endpoint=%x", transfer->bEndpointAddress);
  }
  else if (transfer->status == USB_TRANSFER_STATUS_STALL)
  {
    ESP_LOGI("EspUsbHost", "Transfer STALL Received: Endpoint=%x", transfer->bEndpointAddress);
  }
  else
  {
    ESP_LOGI("EspUsbHost", "Transfer Error or Incomplete: Status=%x, Endpoint=%x", transfer->status, transfer->bEndpointAddress);
  }

  if (!usbHost->deviceSuspended)
  {
    usb_host_transfer_submit(transfer);
  }
}

void EspUsbHost::onMouse(hid_mouse_report_t report, uint8_t last_buttons)
{
  ESP_LOGD("EspUsbHost", "last_buttons=0x%02x(%c%c%c%c%c), buttons=0x%02x(%c%c%c%c%c), x=%d, y=%d, wheel=%d",
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
      process_TX("km.left(1)\n");
    }
    if ((last_buttons & MOUSE_BUTTON_LEFT) && !(report.buttons & MOUSE_BUTTON_LEFT))
    {
      process_TX("km.left(0)\n");
    }

    if (!(last_buttons & MOUSE_BUTTON_RIGHT) && (report.buttons & MOUSE_BUTTON_RIGHT))
    {
      process_TX("km.right(1)\n");
    }
    if ((last_buttons & MOUSE_BUTTON_RIGHT) && !(report.buttons & MOUSE_BUTTON_RIGHT))
    {
      process_TX("km.right(0)\n");
    }

    if (!(last_buttons & MOUSE_BUTTON_MIDDLE) && (report.buttons & MOUSE_BUTTON_MIDDLE))
    {
      process_TX("km.middle(1)\n");
    }
    if ((last_buttons & MOUSE_BUTTON_MIDDLE) && !(report.buttons & MOUSE_BUTTON_MIDDLE))
    {
      process_TX("km.middle(0)\n");
    }

    if (!(last_buttons & MOUSE_BUTTON_FORWARD) && (report.buttons & MOUSE_BUTTON_FORWARD))
    {
      process_TX("km.side1(1)\n");
    }
    if ((last_buttons & MOUSE_BUTTON_FORWARD) && !(report.buttons & MOUSE_BUTTON_FORWARD))
    {
      process_TX("km.side1(0)\n");
    }

    if (!(last_buttons & MOUSE_BUTTON_BACKWARD) && (report.buttons & MOUSE_BUTTON_BACKWARD))
    {
      process_TX("km.side2(1)\n");
    }
    if ((last_buttons & MOUSE_BUTTON_BACKWARD) && !(report.buttons & MOUSE_BUTTON_BACKWARD))
    {
      process_TX("km.side2(0)\n");
    }
  }
}

void EspUsbHost::onMouseMove(hid_mouse_report_t report)
{
  if (deviceMouseReady)
  {
    if (report.wheel != 0)
    {
      process_TX("km.wheel(%d)\n", report.wheel);
    }
    else
    {
      process_TX("km.move(%d,%d)\n", report.x, report.y);
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
  usb_host_transfer_alloc(wDescriptorLength + 8 + 1, 0, &transfer);

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

  esp_err_t err = usb_host_transfer_submit_control(clientHandle, transfer);
  if (err != ESP_OK)
  {
    ESP_LOGI("EspUsbHost", "usb_host_transfer_submit_control() err=%x", err);
  }
  return err;
}

void EspUsbHost::_onReceiveControl(usb_transfer_t *transfer)
{
  bool isMouse = false;
  uint8_t *p = &transfer->data_buffer[8];
  int totalBytes = transfer->actual_num_bytes;

  // Check for the specific sequence indicating a mouse device
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
    usb_host_transfer_free(transfer);
    return;
  }

  HIDReportDescriptor descriptor = parseHIDReportDescriptor(&transfer->data_buffer[8], transfer->actual_num_bytes - 8);

  usb_host_transfer_free(transfer);
}

static uint8_t getItemSize(uint8_t prefix)
{
  return (prefix & 0x03) + 1;
}

static uint8_t getItemType(uint8_t prefix)
{
  return prefix & 0xFC;
}

EspUsbHost::HIDReportDescriptor EspUsbHost::parseHIDReportDescriptor(uint8_t *data, int length)
{
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
    return value;
  };

  HIDReportDescriptor localHIDReportDesc = {0};

  while (i < length)
  {
    uint8_t prefix = data[i];
    parsedValues.size = (prefix & 0x03);
    parsedValues.size = (parsedValues.size == 3) ? 4 : parsedValues.size;
    uint8_t item = prefix & 0xFC;
    bool isSigned = (item == 0x14 || item == 0x24);
    int16_t value = getValue(data + i + 1, parsedValues.size, isSigned);

    switch (item)
    {
    case 0x04: // USAGE_PAGE
      parsedValues.usagePage = (uint8_t)value;
      break;
    case 0x08: // USAGE
      parsedValues.usage = (uint8_t)value;
      break;
    case 0x84: // REPORT_ID
      parsedValues.reportId = value;
      localHIDReportDesc.reportId = parsedValues.reportId;
      parsedValues.hasReportId = true;
      parsedValues.currentBitOffset += 8;
      break;
    case 0x74: // REPORT_SIZE
      parsedValues.reportSize = value;
      break;
    case 0x94: // REPORT_COUNT
      parsedValues.reportCount = value;
      break;
    case 0x14: // LOGICAL_MINIMUM
      if (parsedValues.size == 1)
      {
        parsedValues.logicalMin8 = (int8_t)value;
      }
      else
      {
        parsedValues.logicalMin16 = value;
      }
      break;
    case 0x24: // LOGICAL_MAXIMUM
      if (parsedValues.size == 1)
      {
        parsedValues.logicalMax8 = (int8_t)value;
      }
      else
      {
        parsedValues.logicalMax = value;
      }
      break;
    case 0xA0: // COLLECTION
      parsedValues.level++;
      parsedValues.collection = value;
      break;
    case 0xC0: // END_COLLECTION
      parsedValues.level--;
      break;
    case 0x80: // INPUT
      // Determine what type of input we are dealing with based on USAGE
      if (parsedValues.usagePage == 0x01 && (parsedValues.usage == 0x30 || parsedValues.usage == 0x31))
      { // X-axis or Y-axis
        if (parsedValues.logicalMax <= 2047)
        {
          // Handle 12-bit range
          localHIDReportDesc.xAxisSize = 12;
          localHIDReportDesc.yAxisSize = 12;
          localHIDReportDesc.xAxisStartByte = parsedValues.currentBitOffset / 8;
          parsedValues.currentBitOffset += 12;
          localHIDReportDesc.yAxisStartByte = parsedValues.currentBitOffset / 8;
          parsedValues.currentBitOffset += 12;
        }
        else
        {
          // Handle 8-bit or 16-bit ranges
          localHIDReportDesc.xAxisSize = localHIDReportDesc.yAxisSize =
              (parsedValues.logicalMax <= 127) ? 8 : 16;
          localHIDReportDesc.xAxisStartByte = parsedValues.currentBitOffset / 8;
          parsedValues.currentBitOffset += localHIDReportDesc.xAxisSize;
          localHIDReportDesc.yAxisStartByte = parsedValues.currentBitOffset / 8;
          parsedValues.currentBitOffset += localHIDReportDesc.yAxisSize;
        }
      }
      else if (parsedValues.usagePage == 0x01 && parsedValues.usage == 0x38)
      {
        localHIDReportDesc.wheelSize = (parsedValues.logicalMax <= 127 && parsedValues.logicalMax >= -128) ? 8 : 16;
        localHIDReportDesc.wheelStartByte = parsedValues.currentBitOffset / 8;
        parsedValues.currentBitOffset += localHIDReportDesc.wheelSize;
      }
      else if (parsedValues.usagePage == 0x09 && parsedValues.usage >= 0x01 && parsedValues.usage <= 0x10)
      { // Buttons
        localHIDReportDesc.buttonSize = parsedValues.reportCount * parsedValues.reportSize;
        localHIDReportDesc.buttonStartByte = parsedValues.currentBitOffset / 8;
        parsedValues.currentBitOffset += localHIDReportDesc.buttonSize;
      }
      break;
    case 0x18: // USAGE_MINIMUM
      parsedValues.usageMinimum = (uint8_t)value;
      break;
    case 0x28: // USAGE_MAXIMUM
      parsedValues.usageMaximum = (uint8_t)value;
      break;
    default:
      break;
    }

    i += parsedValues.size + 1;
  }

  HIDReportDesc = localHIDReportDesc;

  const char *TAG = "HIDReportDescriptor";
  ESP_LOGI(TAG, "HID Report Descriptor:");
  ESP_LOGI(TAG, "Report ID: %02X", HIDReportDesc.reportId);
  ESP_LOGI(TAG, "Button Bitmap Size: %d", HIDReportDesc.buttonSize);
  ESP_LOGI(TAG, "X-Axis Size: %d", HIDReportDesc.xAxisSize);
  ESP_LOGI(TAG, "Y-Axis Size: %d", HIDReportDesc.yAxisSize);
  ESP_LOGI(TAG, "Wheel Size: %d", HIDReportDesc.wheelSize);
  ESP_LOGI(TAG, "Button Start Byte: %d", HIDReportDesc.buttonStartByte);
  ESP_LOGI(TAG, "X Axis Start Byte: %d", HIDReportDesc.xAxisStartByte);
  ESP_LOGI(TAG, "Y Axis Start Byte: %d", HIDReportDesc.yAxisStartByte);
  ESP_LOGI(TAG, "Wheel Start Byte: %d", HIDReportDesc.wheelStartByte);

  return HIDReportDesc;
}




void ledFlashTask(void *parameter) {
    digitalWrite(9, HIGH);

    vTaskDelay(ledFlashTime / portTICK_PERIOD_MS);

    digitalWrite(9, LOW);

    vTaskDelay(ledFlashTime / portTICK_PERIOD_MS);

    bool *taskRunning = (bool *)parameter; 
    *taskRunning = false;

    vTaskDelete(NULL);
}

void flashLED() {

    static bool ledFlashTaskRunning = false;


    if (!ledFlashTaskRunning) {
        ledFlashTaskRunning = true;
        xTaskCreate(ledFlashTask, "LED Flash Task", 1024, &ledFlashTaskRunning, 1, NULL);
    }
}

// DeviceInfo structure
void EspUsbHost::sendDeviceInfo()
{
  Serial1.print("USB_sendDeviceInfo:");
  JsonDocument doc;
  doc["speed"] = device_info.speed;
  doc["dev_addr"] = device_info.dev_addr;
  doc["vMaxPacketSize0"] = device_info.vMaxPacketSize0;
  doc["bConfigurationValue"] = device_info.bConfigurationValue;
  doc["str_desc_manufacturer"] = device_info.str_desc_manufacturer;
  doc["str_desc_product"] = device_info.str_desc_product;
  doc["str_desc_serial_num"] = device_info.str_desc_serial_num;
  serializeJson(doc, Serial1);
  Serial1.println();
}

// DescriptorDevice structure
void EspUsbHost::sendDescriptorDevice()
{
  Serial1.print("USB_sendDescriptorDevice:");
  JsonDocument doc;
  doc["bLength"] = descriptor_device.bLength;
  doc["bDescriptorType"] = descriptor_device.bDescriptorType;
  doc["bcdUSB"] = descriptor_device.bcdUSB;
  doc["bDeviceClass"] = descriptor_device.bDeviceClass;
  doc["bDeviceSubClass"] = descriptor_device.bDeviceSubClass;
  doc["bDeviceProtocol"] = descriptor_device.bDeviceProtocol;
  doc["bMaxPacketSize0"] = descriptor_device.bMaxPacketSize0;
  doc["idVendor"] = descriptor_device.idVendor;
  doc["idProduct"] = descriptor_device.idProduct;
  doc["bcdDevice"] = descriptor_device.bcdDevice;
  doc["iManufacturer"] = descriptor_device.iManufacturer;
  doc["iProduct"] = descriptor_device.iProduct;
  doc["iSerialNumber"] = descriptor_device.iSerialNumber;
  doc["bNumConfigurations"] = descriptor_device.bNumConfigurations;
  serializeJson(doc, Serial1);
  Serial1.println();
}

void EspUsbHost::sendEndpointDescriptors()
{
  Serial1.print("USB_sendEndpointDescriptors:");
  JsonDocument doc;
  JsonArray array = doc.to<JsonArray>();

  for (int i = 0; i < endpointCounter; ++i)
  {
    JsonObject desc = array.add<JsonObject>();
    desc["bLength"] = endpoint_descriptors[i].bLength;
    desc["bDescriptorType"] = endpoint_descriptors[i].bDescriptorType;
    desc["bEndpointAddress"] = endpoint_descriptors[i].bEndpointAddress;
    desc["endpointID"] = endpoint_descriptors[i].endpointID;
    desc["direction"] = endpoint_descriptors[i].direction;
    desc["bmAttributes"] = endpoint_descriptors[i].bmAttributes;
    desc["attributes"] = endpoint_descriptors[i].attributes;
    desc["wMaxPacketSize"] = endpoint_descriptors[i].wMaxPacketSize;
    desc["bInterval"] = endpoint_descriptors[i].bInterval;
  }
  serializeJson(doc, Serial1);
  Serial1.println();
}

void EspUsbHost::sendInterfaceDescriptors()
{
  Serial1.print("USB_sendInterfaceDescriptors:");
  JsonDocument doc;
  JsonArray array = doc.to<JsonArray>();
  for (int i = 0; i < interfaceCounter; ++i)
  {
    JsonObject desc = array.add<JsonObject>();
    desc["bLength"] = interface_descriptors[i].bLength;
    desc["bDescriptorType"] = interface_descriptors[i].bDescriptorType;
    desc["bInterfaceNumber"] = interface_descriptors[i].bInterfaceNumber;
    desc["bAlternateSetting"] = interface_descriptors[i].bAlternateSetting;
    desc["bNumEndpoints"] = interface_descriptors[i].bNumEndpoints;
    desc["bInterfaceClass"] = interface_descriptors[i].bInterfaceClass;
    desc["bInterfaceSubClass"] = interface_descriptors[i].bInterfaceSubClass;
    desc["bInterfaceProtocol"] = interface_descriptors[i].bInterfaceProtocol;
    desc["iInterface"] = interface_descriptors[i].iInterface;
  }
  serializeJson(doc, Serial1);
  Serial1.println();
}

void EspUsbHost::sendHidDescriptors()
{
  Serial1.print("USB_sendHidDescriptors:");
  JsonDocument doc;
  JsonArray array = doc.to<JsonArray>();
  for (int i = 0; i < hidDescriptorCounter; ++i)
  {
    JsonObject desc = array.add<JsonObject>();
    desc["bLength"] = hid_descriptors[i].bLength;
    desc["bDescriptorType"] = hid_descriptors[i].bDescriptorType;
    desc["bcdHID"] = hid_descriptors[i].bcdHID;
    desc["bCountryCode"] = hid_descriptors[i].bCountryCode;
    desc["bNumDescriptors"] = hid_descriptors[i].bNumDescriptors;
    desc["bReportType"] = hid_descriptors[i].bReportType;
    desc["wReportLength"] = hid_descriptors[i].wReportLength;
  }
  serializeJson(doc, Serial1);
  Serial1.println();
}

// Interface Association Descriptor structure
void EspUsbHost::sendIADescriptors()
{
  Serial1.print("USB_sendIADescriptors:");
  JsonDocument doc;
  doc["bLength"] = descriptor_interface_association.bLength;
  doc["bDescriptorType"] = descriptor_interface_association.bDescriptorType;
  doc["bFirstInterface"] = descriptor_interface_association.bFirstInterface;
  doc["bInterfaceCount"] = descriptor_interface_association.bInterfaceCount;
  doc["bFunctionClass"] = descriptor_interface_association.bFunctionClass;
  doc["bFunctionSubClass"] = descriptor_interface_association.bFunctionSubClass;
  doc["bFunctionProtocol"] = descriptor_interface_association.bFunctionProtocol;
  doc["iFunction"] = descriptor_interface_association.iFunction;
  serializeJson(doc, Serial1);
  Serial1.println();
}

void EspUsbHost::sendEndpointData()
{
  Serial1.print("USB_sendEndpointData:");
  JsonDocument doc;
  JsonArray array = doc.to<JsonArray>();

  for (int i = 0; i < interfaceCounter; i++)
  {
    JsonObject data = array.add<JsonObject>();
    data["bInterfaceNumber"] = endpoint_data_list[i].bInterfaceNumber;
    data["bInterfaceClass"] = endpoint_data_list[i].bInterfaceClass;
    data["bInterfaceSubClass"] = endpoint_data_list[i].bInterfaceSubClass;
    data["bInterfaceProtocol"] = endpoint_data_list[i].bInterfaceProtocol;
    data["bCountryCode"] = endpoint_data_list[i].bCountryCode;
  }
  serializeJson(doc, Serial1);
  Serial1.println();
}

void EspUsbHost::sendUnknownDescriptors()
{
  Serial1.print("USB_sendUnknownDescriptors:");
  JsonDocument doc;
  JsonArray array = doc.to<JsonArray>();
  for (int i = 0; i < unknownDescriptorCounter; ++i)
  {
    JsonObject desc = array.add<JsonObject>();
    desc["bLength"] = unknown_descriptors[i].bLength;
    desc["bDescriptorType"] = unknown_descriptors[i].bDescriptorType;
    desc["data"] = unknown_descriptors[i].data;
  }
  serializeJson(doc, Serial1);
  Serial1.println();
}

void EspUsbHost::sendDescriptorconfig()
{
  Serial1.print("USB_sendDescriptorconfig:");
  JsonDocument doc;
  doc["bLength"] = descriptor_configuration.bLength;
  doc["bDescriptorType"] = descriptor_configuration.bDescriptorType;
  doc["wTotalLength"] = descriptor_configuration.wTotalLength;
  doc["bNumInterfaces"] = descriptor_configuration.bNumInterfaces;
  doc["bConfigurationValue"] = descriptor_configuration.bConfigurationValue;
  doc["iConfiguration"] = descriptor_configuration.iConfiguration;
  doc["bmAttributes"] = descriptor_configuration.bmAttributes;
  doc["bMaxPower"] = descriptor_configuration.bMaxPower;
  serializeJson(doc, Serial1);
  Serial1.println();
}

void EspUsbHost::handleIncomingCommands(const String &command)
{
  if (command == "READY")
  {
    // vTaskDelay(200); // give time for the USB device to init
    if (EspUsbHost::deviceConnected)
    {
      process_TX("USB_HELLO\n");
    }
    else
    {
      process_TX("USB_ISNULL\n");
    }
  }
  else if (command == "USB_INIT")
  {
    deviceMouseReady = true;
  }
  else if (command == "sendDeviceInfo")
  {
    sendDeviceInfo();
  }
  else if (command == "sendDescriptorDevice")
  {
    sendDescriptorDevice();
  }
  else if (command == "sendEndpointDescriptors")
  {
    sendEndpointDescriptors();
  }
  else if (command == "sendInterfaceDescriptors")
  {
    sendInterfaceDescriptors();
  }
  else if (command == "sendHidDescriptors")
  {
    sendHidDescriptors();
  }
  else if (command == "sendIADescriptors")
  {
    sendIADescriptors();
  }
  else if (command == "sendEndpointData")
  {
    sendEndpointData();
  }
  else if (command == "sendUnknownDescriptors")
  {
    sendUnknownDescriptors();
  }
  else if (command == "sendDescriptorconfig")
  {
    sendDescriptorconfig();
  }
  else if (command == "YIELD")
  {
    enableYield = true;
  }
  else if (command == "YIELD_END")
  {
    enableYield = false;
  }
  else
  {
    Serial1.println("Unknown command received: " + command);
  }
}


