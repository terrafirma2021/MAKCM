
#include "EspUsbHost.h"
#include "esp_task_wdt.h"
#include <driver/timer.h>
#include "freertos/queue.h"
#include "esp_log.h"
#include <stdarg.h>
#include <stdio.h>
#include <sstream>
#include <iomanip>
#include "freertos/semphr.h"


extern QueueHandle_t logQueue;
int EspUsbHost::current_log_level = LOG_LEVEL_OFF;
bool EspUsbHost::deviceMouseReady = false; 


void EspUsbHost::log(int level, const char *format, ...)
{
    // Ensure logging only occurs for LOG_LEVEL_FIXED or lower, and skip if the current log level is LOG_LEVEL_OFF
    if (level > LOG_LEVEL_FIXED || EspUsbHost::current_log_level == LOG_LEVEL_OFF)
    {
        return;
    }

    char logMsg[MaxLogQueSize];
    va_list args;
    va_start(args, format);
    int logLength = vsnprintf(logMsg, sizeof(logMsg), format, args);
    va_end(args);

    if (!tx_Que(logMsg))
    {
        Serial1.println("Error: Failed to send log message via tx_Que.");
    }
}




void EspUsbHost::logRawBytes(const uint8_t *data, size_t length, const std::string &label)
{
    if (EspUsbHost::current_log_level < LOG_LEVEL_FIXED)
    {
        return;
    }

    char logBuffer[MaxLogQueSize];
    int logIndex = snprintf(logBuffer, sizeof(logBuffer), "**%s**\n", label.c_str());

    if (logIndex >= MaxLogQueSize)
    {
        Serial1.println("Error: Log buffer full while writing the label.");
        return;
    }

    for (size_t i = 0; i < length && logIndex < MaxLogQueSize - 3; ++i)
    {
        logIndex += snprintf(logBuffer + logIndex, sizeof(logBuffer) - logIndex, "%02X ", data[i]);

        if (logIndex >= MaxLogQueSize)
        {
            Serial1.println("Error: Log buffer full while writing raw bytes.");
            return;
        }
    }

    for (size_t i = length; i < 10 && logIndex < MaxLogQueSize - 3; ++i)
    {
        logIndex += snprintf(logBuffer + logIndex, sizeof(logBuffer) - logIndex, "00 ");

        if (logIndex >= MaxLogQueSize)
        {
            Serial1.println("Error: Log buffer full while padding with zeros.");
            return;
        }
    }

    logIndex += snprintf(logBuffer + logIndex, sizeof(logBuffer) - logIndex, "\n");

    if (logIndex >= MaxLogQueSize)
    {
        Serial1.println("Error: Log buffer full while adding newline.");
        return;
    }

    if (!tx_Que(logBuffer))
    {
        Serial1.println("Error: Failed to send log bytes via tx_Que.");
    }
}



void processLogQueue(void *parameter)
{
    EspUsbHost *instance = static_cast<EspUsbHost *>(parameter);
    char logMsg[MaxLogQueSize];

    while (true)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        while (xQueueReceive(logQueue, logMsg, 0) == pdPASS)
        {
            if (!instance->tx_Que(logMsg))
            {
                Serial1.println("Error: Failed to send log message via tx_Que.");
            }
        }
    }
}

void EspUsbHost::logDeviceDescriptor(const usb_device_desc_t &dev_desc)
{
    if (EspUsbHost::current_log_level != LOG_LEVEL_PARSED)
    {
        return;
    }

    // log(LOG_LEVEL_PARSED, "\n==============================\n");
    log(LOG_LEVEL_PARSED, "**USB Device Descriptor**\n");
    log(LOG_LEVEL_PARSED, "Length: %d\n", dev_desc.bLength);
    log(LOG_LEVEL_PARSED, "Descriptor Type: %d\n", dev_desc.bDescriptorType);
    log(LOG_LEVEL_PARSED, "USB Version: %04X\n", dev_desc.bcdUSB);
    log(LOG_LEVEL_PARSED, "Device Class: %d\n", dev_desc.bDeviceClass);
    log(LOG_LEVEL_PARSED, "Device SubClass: %d\n", dev_desc.bDeviceSubClass);
    log(LOG_LEVEL_PARSED, "Device Protocol: %d\n", dev_desc.bDeviceProtocol);
    log(LOG_LEVEL_PARSED, "Max Packet Size: %d\n", dev_desc.bMaxPacketSize0);
    log(LOG_LEVEL_PARSED, "Vendor ID: %04X\n", dev_desc.idVendor);
    log(LOG_LEVEL_PARSED, "Product ID: %04X\n", dev_desc.idProduct);
    log(LOG_LEVEL_PARSED, "Device Version: %04X\n", dev_desc.bcdDevice);
    log(LOG_LEVEL_PARSED, "Manufacturer Index: %d\n", dev_desc.iManufacturer);
    log(LOG_LEVEL_PARSED, "Product Index: %d\n", dev_desc.iProduct);
    log(LOG_LEVEL_PARSED, "Serial Number Index: %d\n", dev_desc.iSerialNumber);
    log(LOG_LEVEL_PARSED, "Num Configurations: %d\n", dev_desc.bNumConfigurations);
}

void EspUsbHost::logStringDescriptor(const usb_string_descriptor_t &str_desc)
{
    if (LOG_LEVEL_PARSED > EspUsbHost::current_log_level)
    {
        return;
    }

    // log(LOG_LEVEL_PARSED, "\n==============================\n");
    log(LOG_LEVEL_PARSED, "**USB String Descriptor**\n");
    log(LOG_LEVEL_PARSED, "Length: %d\n", str_desc.bLength);
    log(LOG_LEVEL_PARSED, "Descriptor Type: %d\n", str_desc.bDescriptorType);
    log(LOG_LEVEL_PARSED, "String: %s\n", str_desc.data.c_str());
}

void EspUsbHost::logInterfaceDescriptor(const usb_intf_desc_t &intf)
{
    if (LOG_LEVEL_PARSED > EspUsbHost::current_log_level)
    {
        return;
    }

    //   log(LOG_LEVEL_PARSED, "\n==============================\n");
    log(LOG_LEVEL_PARSED, "**USB Interface Descriptor**\n");
    log(LOG_LEVEL_PARSED, "Interface Number: %d\n", intf.bInterfaceNumber);
    log(LOG_LEVEL_PARSED, "Alternate Setting: %d\n", intf.bAlternateSetting);
    log(LOG_LEVEL_PARSED, "Num Endpoints: %d\n", intf.bNumEndpoints);
    log(LOG_LEVEL_PARSED, "Interface Class: %d\n", intf.bInterfaceClass);
    log(LOG_LEVEL_PARSED, "Interface SubClass: %d\n", intf.bInterfaceSubClass);
    log(LOG_LEVEL_PARSED, "Interface Protocol: %d\n", intf.bInterfaceProtocol);
    log(LOG_LEVEL_PARSED, "Interface String Index: %d\n", intf.iInterface);
}

void EspUsbHost::logEndpointDescriptor(const usb_ep_desc_t &ep_desc)
{
    if (LOG_LEVEL_PARSED > EspUsbHost::current_log_level)
    {
        return;
    }

    //   log(LOG_LEVEL_PARSED, "\n==============================\n");
    log(LOG_LEVEL_PARSED, "**USB Endpoint Descriptor**\n");
    log(LOG_LEVEL_PARSED, "Endpoint Address: 0x%02X\n", ep_desc.bEndpointAddress);
    log(LOG_LEVEL_PARSED, "Attributes: 0x%02X\n", ep_desc.bmAttributes);
    log(LOG_LEVEL_PARSED, "Max Packet Size: %d\n", ep_desc.wMaxPacketSize);
    log(LOG_LEVEL_PARSED, "Interval: %d\n", ep_desc.bInterval);
    log(LOG_LEVEL_PARSED, "Direction: %s\n", (ep_desc.bEndpointAddress & 0x80) ? "IN" : "OUT");
    log(LOG_LEVEL_PARSED, "Transfer Type: %s\n",
        (ep_desc.bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) == USB_BM_ATTRIBUTES_XFER_CONTROL ? "Control" : (ep_desc.bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) == USB_BM_ATTRIBUTES_XFER_ISOC ? "Isochronous"
                                                                                                             : (ep_desc.bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) == USB_BM_ATTRIBUTES_XFER_BULK   ? "Bulk"
                                                                                                             : (ep_desc.bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) == USB_BM_ATTRIBUTES_XFER_INT    ? "Interrupt"
                                                                                                                                                                                                         : "Unknown");
}

void EspUsbHost::logInterfaceAssociationDescriptor(const usb_iad_desc_t &iad_desc)
{
    if (LOG_LEVEL_PARSED > EspUsbHost::current_log_level)
    {
        return;
    }

    //  log(LOG_LEVEL_PARSED, "\n==============================\n");
    log(LOG_LEVEL_PARSED, "**USB Interface Association Descriptor**\n");
    log(LOG_LEVEL_PARSED, "Length: %d\n", iad_desc.bLength);
    log(LOG_LEVEL_PARSED, "Descriptor Type: %d\n", iad_desc.bDescriptorType);
    log(LOG_LEVEL_PARSED, "First Interface: %d\n", iad_desc.bFirstInterface);
    log(LOG_LEVEL_PARSED, "Interface Count: %d\n", iad_desc.bInterfaceCount);
    log(LOG_LEVEL_PARSED, "Function Class: %d\n", iad_desc.bFunctionClass);
    log(LOG_LEVEL_PARSED, "Function SubClass: %d\n", iad_desc.bFunctionSubClass);
    log(LOG_LEVEL_PARSED, "Function Protocol: %d\n", iad_desc.bFunctionProtocol);
    log(LOG_LEVEL_PARSED, "Function String Index: %d\n", iad_desc.iFunction);
}

void EspUsbHost::logHIDDescriptor(const tusb_hid_descriptor_hid_t &hid_desc)
{
    if (LOG_LEVEL_PARSED > EspUsbHost::current_log_level)
    {
        return;
    }

    //  log(LOG_LEVEL_PARSED, "\n==============================\n");
    log(LOG_LEVEL_PARSED, "**USB HID Descriptor**\n");
    log(LOG_LEVEL_PARSED, "Length: %d\n", hid_desc.bLength);
    log(LOG_LEVEL_PARSED, "Descriptor Type: %d\n", hid_desc.bDescriptorType);
    log(LOG_LEVEL_PARSED, "HID Class Spec Version: %04X\n", hid_desc.bcdHID);
    log(LOG_LEVEL_PARSED, "Country Code: %d\n", hid_desc.bCountryCode);
    log(LOG_LEVEL_PARSED, "Num Descriptors: %d\n", hid_desc.bNumDescriptors);
    log(LOG_LEVEL_PARSED, "Descriptor Type: %d\n", hid_desc.bReportType);
    log(LOG_LEVEL_PARSED, "Descriptor Length: %d\n", hid_desc.wReportLength);
}

void EspUsbHost::logConfigurationDescriptor(const UsbConfigurationDescriptor &config_desc)
{
    if (LOG_LEVEL_PARSED > EspUsbHost::current_log_level)
    {
        return;
    }

    log(LOG_LEVEL_PARSED, "**Configuration Descriptor**\n");
    log(LOG_LEVEL_PARSED, "Length: %d\n", config_desc.bLength);
    log(LOG_LEVEL_PARSED, "Descriptor Type: %d\n", config_desc.bDescriptorType);
    log(LOG_LEVEL_PARSED, "Total Length: %d\n", config_desc.wTotalLength);
    log(LOG_LEVEL_PARSED, "Number of Interfaces: %d\n", config_desc.bNumInterfaces);
    log(LOG_LEVEL_PARSED, "Configuration Value: %d\n", config_desc.bConfigurationValue);
    log(LOG_LEVEL_PARSED, "Configuration Index: %d\n", config_desc.iConfiguration);
    log(LOG_LEVEL_PARSED, "Attributes: 0x%02X\n", config_desc.bmAttributes);
    log(LOG_LEVEL_PARSED, "Max Power: %d (in 2mA units, so %dmA)\n", config_desc.bMaxPower, config_desc.bMaxPower * 2);
}

void EspUsbHost::logUnknownDescriptor(const usb_standard_desc_t &desc)
{
    if (LOG_LEVEL_PARSED > EspUsbHost::current_log_level)
    {
        return;
    }
    //   log(LOG_LEVEL_PARSED, "\n==============================\n");
    log(LOG_LEVEL_PARSED, "**Unknown Descriptor**\n");
    log(LOG_LEVEL_PARSED, "Length: %d\n", desc.bLength);
    log(LOG_LEVEL_PARSED, "Descriptor Type: %d\n", desc.bDescriptorType);

    std::ostringstream rawDataStream;
    for (int i = 0; i < desc.bLength; ++i)
    {
        rawDataStream << std::hex << std::uppercase << std::setw(2) << std::setfill('0')
                      << static_cast<int>(desc.val[i]) << " ";
    }
}

void EspUsbHost::logHIDReportDescriptor(const HIDReportDescriptor &desc)
{
    if (LOG_LEVEL_FIXED > EspUsbHost::current_log_level)
    {
        return;
    }

    log(LOG_LEVEL_FIXED, "**\n");
    log(LOG_LEVEL_FIXED, "**HID Report Descriptor**\n");
    log(LOG_LEVEL_FIXED, "Report ID: %02X\n", desc.reportId);
    log(LOG_LEVEL_FIXED, "Button Bitmap Size: %d\n", desc.buttonSize);
    log(LOG_LEVEL_FIXED, "X-Axis Size: %d\n", desc.xAxisSize);
    log(LOG_LEVEL_FIXED, "Y-Axis Size: %d\n", desc.yAxisSize);
    log(LOG_LEVEL_FIXED, "Wheel Size: %d\n", desc.wheelSize);
    log(LOG_LEVEL_FIXED, "Button Start Byte: %d\n", desc.buttonStartByte);
    log(LOG_LEVEL_FIXED, "X Axis Start Byte: %d\n", desc.xAxisStartByte);
    log(LOG_LEVEL_FIXED, "Y Axis Start Byte: %d\n", desc.yAxisStartByte);
    log(LOG_LEVEL_FIXED, "Wheel Start Byte: %d\n", desc.wheelStartByte);
}

