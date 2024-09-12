#ifndef ESP_USB_HOST_H
#define ESP_USB_HOST_H

#include <Arduino.h>
#include <usb/usb_host.h>
#include <class/hid/hid.h>
#include <rom/usb/usb_common.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include "freertos/queue.h"
#include <cstring>
#include <ArduinoJson.h>
#include <iomanip>
#include <sstream>
#include <string>
#include <RingBuf.h>

#define LOG_LEVEL_OFF    0
#define LOG_LEVEL_FIXED  1
#define LOG_LEVEL_INFO   2
#define LOG_LEVEL_WARN   3
#define LOG_LEVEL_ERROR  4
#define LOG_LEVEL_DEBUG  5
#define LOG_LEVEL_PARSED 6

#define LOG_QUEUE_SIZE 20
#define LOG_MESSAGE_SIZE 620


void flashLEDToggleTask(void *parameter);
void processLogMessages(void* parameter);
//extern QueueHandle_t logQueue;
//extern int current_log_level;
//extern bool deviceMouseReady;
extern SemaphoreHandle_t ledSemaphore;
extern SemaphoreHandle_t serialMutex;
extern RingBuf<char, 512> logBuffer;   // Log Buffer for log messages
extern RingBuf<char, 620> txBuffer;    // TX Buffer for outgoing data
extern RingBuf<char, 512> rxBuffer;    // RX Buffer for incoming data



class EspUsbHost
{
public:
    // Debug and Log
    bool debugModeActive = false;
    void log(int level, const char *format, ...);
    bool isReady = false;
    static int current_log_level;
    static bool deviceMouseReady;
    //static bool noDevice;
    uint8_t interval;
    bool isClientRegistering = false;
    QueueHandle_t txQueue;
    QueueHandle_t rxQueue;
    TaskHandle_t espTxTaskHandle;
    TaskHandle_t logTaskHandle;

    bool deviceSuspended = false;
    static bool deviceConnected;
    uint32_t last_activity_time;

    enum Strutstate {
        STATE_IDLE,
        STATE_PROCESSING_COMMAND,
        STATE_SENDING_RESPONSE,
    };

    struct usb_string_descriptor_t {
        uint8_t bLength;
        uint8_t bDescriptorType;
        String data;
    };

    struct endpoint_data_t
    {
        uint8_t bInterfaceNumber;
        uint8_t bInterfaceClass;
        uint8_t bInterfaceSubClass;
        uint8_t bInterfaceProtocol;
        uint8_t bCountryCode;
    } endpoint_data_list[17];

    esp_err_t claim_err;
    usb_host_client_handle_t clientHandle;
    usb_device_handle_t deviceHandle;
    usb_transfer_t *usbTransfer[16];
    uint8_t usbTransferSize;
    uint8_t usbInterface[16];
    uint8_t usbInterfaceSize;

    TaskHandle_t usbTaskHandle = nullptr;
    TaskHandle_t clientTaskHandle = nullptr;

    struct HIDReportDescriptor {
        uint8_t reportId;
        uint8_t buttonSize;
        uint8_t xAxisSize;
        uint8_t yAxisSize;
        uint8_t wheelSize;
        uint8_t buttonStartByte;
        uint8_t xAxisStartByte;
        uint8_t yAxisStartByte;
        uint8_t wheelStartByte;
    };

    static struct HIDReportDescriptor HIDReportDesc;

    struct DeviceInfo {
        uint8_t speed;                         // USB device speed
        uint8_t dev_addr;                      // Device address
        uint8_t vMaxPacketSize0;               // Maximum packet size for endpoint 0
        uint8_t bConfigurationValue;           // Configuration value
        char str_desc_manufacturer[64];        // Manufacturer string descriptor
        char str_desc_product[64];             // Product string descriptor
        char str_desc_serial_num[64];          // Serial number string descriptor
    } device_info;

    struct DescriptorDevice {
        uint8_t bLength;                       // Descriptor length
        uint8_t bDescriptorType;               // Descriptor type
        uint16_t bcdUSB;                       // USB specification number
        uint8_t bDeviceClass;                  // Device class
        uint8_t bDeviceSubClass;               // Device subclass
        uint8_t bDeviceProtocol;               // Device protocol
        uint8_t bMaxPacketSize0;               // Maximum packet size for endpoint 0
        uint16_t idVendor;                     // Vendor ID
        uint16_t idProduct;                    // Product ID
        uint16_t bcdDevice;                    // Device release number
        uint8_t iManufacturer;                 // Index of manufacturer string descriptor
        uint8_t iProduct;                      // Index of product string descriptor
        uint8_t iSerialNumber;                 // Index of serial number string descriptor
        uint8_t bNumConfigurations;            // Number of possible configurations
    } descriptor_device;

    #define MAX_ENDPOINT_DESCRIPTORS 10
    struct usb_endpoint_descriptor_t {
        uint8_t bLength;           
        uint8_t bDescriptorType;   
        uint8_t bEndpointAddress;  
        uint8_t endpointID;        
        String direction;          
        uint8_t bmAttributes;      
        String attributes;        
        uint16_t wMaxPacketSize;   
        uint8_t bInterval;         
    } endpoint_descriptors[MAX_ENDPOINT_DESCRIPTORS];
    int endpointCounter = 0; 

    struct DescriptorConfiguration {
        uint8_t bLength;             // Descriptor length
        uint8_t bDescriptorType;     // Descriptor type
        uint16_t wTotalLength;       // Total length of data for this configuration
        uint8_t bNumInterfaces;      // Number of interfaces
        uint8_t bConfigurationValue; // Configuration value
        uint8_t iConfiguration;      // Index of string descriptor describing this configuration
        uint8_t bmAttributes;        // Configuration characteristics
        uint8_t bMaxPower;           // Maximum power consumption (in 2mA units)
    } descriptor_configuration;

    #define MAX_INTERFACE_DESCRIPTORS 10
    struct usb_interface_descriptor_t {
        uint8_t bLength;           
        uint8_t bDescriptorType;   
        uint8_t bInterfaceNumber;  
        uint8_t bAlternateSetting; 
        uint8_t bNumEndpoints;     
        uint8_t bInterfaceClass;   
        uint8_t bInterfaceSubClass;
        uint8_t bInterfaceProtocol;
        uint8_t iInterface;
    } interface_descriptors[MAX_INTERFACE_DESCRIPTORS];
    int interfaceCounter = 0;

    #define MAX_HID_DESCRIPTORS 10 
    struct usb_hid_descriptor_t {
        uint8_t bLength;        
        uint8_t bDescriptorType;
        uint16_t bcdHID;        
        uint8_t bCountryCode;   
        uint8_t bNumDescriptors;
        uint8_t bReportType;
        uint16_t wReportLength;
    } hid_descriptors[MAX_HID_DESCRIPTORS];
    int hidDescriptorCounter = 0;

    struct usb_iad_desc_t {
        uint8_t bLength;
        uint8_t bDescriptorType;
        uint8_t bFirstInterface;
        uint8_t bInterfaceCount;
        uint8_t bFunctionClass;
        uint8_t bFunctionSubClass;
        uint8_t bFunctionProtocol;
        uint8_t iFunction;
    } descriptor_interface_association;

 struct UsbConfigurationDescriptor {
        uint8_t bLength;
        uint8_t bDescriptorType;
        uint16_t wTotalLength;
        uint8_t bNumInterfaces;
        uint8_t bConfigurationValue;
        uint8_t iConfiguration;
        uint8_t bmAttributes;
        uint8_t bMaxPower;
    } configurationDescriptor;

    #define MAX_UNKNOWN_DESCRIPTORS 10

    struct usb_unknown_descriptor_t {
        uint8_t bLength;
        uint8_t bDescriptorType;
        String data;
    } unknown_descriptors[MAX_UNKNOWN_DESCRIPTORS];
    int unknownDescriptorCounter = 0;

    struct ParsedValues
    {
        uint8_t usagePage;
        uint8_t usage;
        uint8_t reportId;
        uint8_t reportSize;
        uint8_t reportCount;
        int16_t logicalMin16;
        int16_t logicalMax;
        int8_t logicalMin8;
        int8_t logicalMax8;
        uint8_t level;
        uint8_t size;
        uint8_t collection;
        uint16_t usageMinimum;
        uint16_t usageMaximum;
        bool hasReportId;
        uint16_t currentBitOffset;
    };

    struct LogDeviceDescriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t bcdUSB;
    uint8_t bDeviceClass;
    uint8_t bDeviceSubClass;
    uint8_t bDeviceProtocol;
    uint8_t bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t iManufacturer;
    uint8_t iProduct;
    uint8_t iSerialNumber;
    uint8_t bNumConfigurations;

    void appendFromAPI(const usb_device_desc_t &dev_desc) {
        bLength = dev_desc.bLength;
        bDescriptorType = dev_desc.bDescriptorType;
        bcdUSB = dev_desc.bcdUSB;
        bDeviceClass = dev_desc.bDeviceClass;
        bDeviceSubClass = dev_desc.bDeviceSubClass;
        bDeviceProtocol = dev_desc.bDeviceProtocol;
        bMaxPacketSize0 = dev_desc.bMaxPacketSize0;
        idVendor = dev_desc.idVendor;
        idProduct = dev_desc.idProduct;
        bcdDevice = dev_desc.bcdDevice;
        iManufacturer = dev_desc.iManufacturer;
        iProduct = dev_desc.iProduct;
        iSerialNumber = dev_desc.iSerialNumber;
        bNumConfigurations = dev_desc.bNumConfigurations;
    }

    // Call this later when you want to log it
    void logPrint(EspUsbHost &host) const {
        host.log(LOG_LEVEL_PARSED, "**USB Device Descriptor**\n");
        host.log(LOG_LEVEL_PARSED, "Length: %d\n", bLength);
        host.log(LOG_LEVEL_PARSED, "Descriptor Type: %d\n", bDescriptorType);
        host.log(LOG_LEVEL_PARSED, "USB Version: %04X\n", bcdUSB);
        host.log(LOG_LEVEL_PARSED, "Device Class: %d\n", bDeviceClass);
        host.log(LOG_LEVEL_PARSED, "Device SubClass: %d\n", bDeviceSubClass);
        host.log(LOG_LEVEL_PARSED, "Device Protocol: %d\n", bDeviceProtocol);
        host.log(LOG_LEVEL_PARSED, "Max Packet Size: %d\n", bMaxPacketSize0);
        host.log(LOG_LEVEL_PARSED, "Vendor ID: %04X\n", idVendor);
        host.log(LOG_LEVEL_PARSED, "Product ID: %04X\n", idProduct);
        host.log(LOG_LEVEL_PARSED, "Device Version: %04X\n", bcdDevice);
        host.log(LOG_LEVEL_PARSED, "Manufacturer Index: %d\n", iManufacturer);
        host.log(LOG_LEVEL_PARSED, "Product Index: %d\n", iProduct);
        host.log(LOG_LEVEL_PARSED, "Serial Number Index: %d\n", iSerialNumber);
        host.log(LOG_LEVEL_PARSED, "Num Configurations: %d\n", bNumConfigurations);
    }
};

struct LogStringDescriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    std::string data;

void appendFromAPI(const EspUsbHost::usb_string_descriptor_t &str_desc) {
    bLength = str_desc.bLength;
    bDescriptorType = str_desc.bDescriptorType;
    // Convert Arduino String (str_desc.data) to std::string
    data = std::string(str_desc.data.c_str());  // Use c_str() to convert to a C-style string first
}

   void logPrint(EspUsbHost &host) const {
        host.log(LOG_LEVEL_PARSED, "**USB String Descriptor**\n");
        host.log(LOG_LEVEL_PARSED, "Length: %d\n", bLength);
        host.log(LOG_LEVEL_PARSED, "Descriptor Type: %d\n", bDescriptorType);
        host.log(LOG_LEVEL_PARSED, "String: %s\n", data.c_str());
    }
};

struct LogInterfaceDescriptor {
    uint8_t bInterfaceNumber;
    uint8_t bAlternateSetting;
    uint8_t bNumEndpoints;
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t iInterface;

    void appendFromAPI(const usb_intf_desc_t &intf) {
        bInterfaceNumber = intf.bInterfaceNumber;
        bAlternateSetting = intf.bAlternateSetting;
        bNumEndpoints = intf.bNumEndpoints;
        bInterfaceClass = intf.bInterfaceClass;
        bInterfaceSubClass = intf.bInterfaceSubClass;
        bInterfaceProtocol = intf.bInterfaceProtocol;
        iInterface = intf.iInterface;
    }

    void logPrint(EspUsbHost &host) const {
        host.log(LOG_LEVEL_PARSED, "**USB Interface Descriptor**\n");
        host.log(LOG_LEVEL_PARSED, "Interface Number: %d\n", bInterfaceNumber);
        host.log(LOG_LEVEL_PARSED, "Alternate Setting: %d\n", bAlternateSetting);
        host.log(LOG_LEVEL_PARSED, "Num Endpoints: %d\n", bNumEndpoints);
        host.log(LOG_LEVEL_PARSED, "Interface Class: %d\n", bInterfaceClass);
        host.log(LOG_LEVEL_PARSED, "Interface SubClass: %d\n", bInterfaceSubClass);
        host.log(LOG_LEVEL_PARSED, "Interface Protocol: %d\n", bInterfaceProtocol);
        host.log(LOG_LEVEL_PARSED, "Interface String Index: %d\n", iInterface);
    }
};

struct LogEndpointDescriptor {
    uint8_t bEndpointAddress;
    uint8_t bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t bInterval;

    void appendFromAPI(const usb_ep_desc_t &ep_desc) {
        bEndpointAddress = ep_desc.bEndpointAddress;
        bmAttributes = ep_desc.bmAttributes;
	        wMaxPacketSize = ep_desc.wMaxPacketSize;
        bInterval = ep_desc.bInterval;
    }

    void logPrint(EspUsbHost &host) const {
        host.log(LOG_LEVEL_PARSED, "**USB Endpoint Descriptor**\n");
        host.log(LOG_LEVEL_PARSED, "Endpoint Address: 0x%02X\n", bEndpointAddress);
        host.log(LOG_LEVEL_PARSED, "Attributes: 0x%02X\n", bmAttributes);
        host.log(LOG_LEVEL_PARSED, "Max Packet Size: %d\n", wMaxPacketSize);
        host.log(LOG_LEVEL_PARSED, "Interval: %d\n", bInterval);
        host.log(LOG_LEVEL_PARSED, "Direction: %s\n", (bEndpointAddress & 0x80) ? "IN" : "OUT");
        host.log(LOG_LEVEL_PARSED, "Transfer Type: %s\n",
            (bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) == USB_BM_ATTRIBUTES_XFER_CONTROL ? "Control" :
            (bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) == USB_BM_ATTRIBUTES_XFER_ISOC    ? "Isochronous" :
            (bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) == USB_BM_ATTRIBUTES_XFER_BULK    ? "Bulk" :
            (bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) == USB_BM_ATTRIBUTES_XFER_INT     ? "Interrupt" : "Unknown");
    }
};

struct LogInterfaceAssociationDescriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bFirstInterface;
    uint8_t bInterfaceCount;
    uint8_t bFunctionClass;
    uint8_t bFunctionSubClass;
    uint8_t bFunctionProtocol;
    uint8_t iFunction;

    void appendFromAPI(const usb_iad_desc_t &iad_desc) {
        bLength = iad_desc.bLength;
        bDescriptorType = iad_desc.bDescriptorType;
        bFirstInterface = iad_desc.bFirstInterface;
        bInterfaceCount = iad_desc.bInterfaceCount;
        bFunctionClass = iad_desc.bFunctionClass;
        bFunctionSubClass = iad_desc.bFunctionSubClass;
        bFunctionProtocol = iad_desc.bFunctionProtocol;
        iFunction = iad_desc.iFunction;
    }

    void logPrint(EspUsbHost &host) const {
        host.log(LOG_LEVEL_PARSED, "**USB Interface Association Descriptor**\n");
        host.log(LOG_LEVEL_PARSED, "Length: %d\n", bLength);
        host.log(LOG_LEVEL_PARSED, "Descriptor Type: %d\n", bDescriptorType);
        host.log(LOG_LEVEL_PARSED, "First Interface: %d\n", bFirstInterface);
        host.log(LOG_LEVEL_PARSED, "Interface Count: %d\n", bInterfaceCount);
        host.log(LOG_LEVEL_PARSED, "Function Class: %d\n", bFunctionClass);
        host.log(LOG_LEVEL_PARSED, "Function SubClass: %d\n", bFunctionSubClass);
        host.log(LOG_LEVEL_PARSED, "Function Protocol: %d\n", bFunctionProtocol);
        host.log(LOG_LEVEL_PARSED, "Function String Index: %d\n", iFunction);
    }
};

struct LogHIDDescriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t bcdHID;
    uint8_t bCountryCode;
    uint8_t bNumDescriptors;
    uint8_t bReportType;
    uint16_t wReportLength;

    void appendFromAPI(const tusb_hid_descriptor_hid_t &hid_desc) {
        bLength = hid_desc.bLength;
        bDescriptorType = hid_desc.bDescriptorType;
        bcdHID = hid_desc.bcdHID;
        bCountryCode = hid_desc.bCountryCode;
        bNumDescriptors = hid_desc.bNumDescriptors;
        bReportType = hid_desc.bReportType;
        wReportLength = hid_desc.wReportLength;
    }

    void logPrint(EspUsbHost &host) const {
        host.log(LOG_LEVEL_PARSED, "**USB HID Descriptor**\n");
        host.log(LOG_LEVEL_PARSED, "Length: %d\n", bLength);
        host.log(LOG_LEVEL_PARSED, "Descriptor Type: %d\n", bDescriptorType);
        host.log(LOG_LEVEL_PARSED, "HID Class Spec Version: %04X\n", bcdHID);
        host.log(LOG_LEVEL_PARSED, "Country Code: %d\n", bCountryCode);
        host.log(LOG_LEVEL_PARSED, "Num Descriptors: %d\n", bNumDescriptors);
        host.log(LOG_LEVEL_PARSED, "Descriptor Type: %d\n", bReportType);
        host.log(LOG_LEVEL_PARSED, "Descriptor Length: %d\n", wReportLength);
    }
};

struct LogConfigurationDescriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t wTotalLength;
    uint8_t bNumInterfaces;
    uint8_t bConfigurationValue;
    uint8_t iConfiguration;
    uint8_t bmAttributes;
    uint8_t bMaxPower;

    void appendFromAPI(const EspUsbHost::UsbConfigurationDescriptor &config_desc) {
        bLength = config_desc.bLength;
        bDescriptorType = config_desc.bDescriptorType;
        wTotalLength = config_desc.wTotalLength;
        bNumInterfaces = config_desc.bNumInterfaces;
        bConfigurationValue = config_desc.bConfigurationValue;
        iConfiguration = config_desc.iConfiguration;
        bmAttributes = config_desc.bmAttributes;
        bMaxPower = config_desc.bMaxPower;
    }

    void logPrint(EspUsbHost &host) const {
        host.log(LOG_LEVEL_PARSED, "**USB Configuration Descriptor**\n");
        host.log(LOG_LEVEL_PARSED, "Length: %d\n", bLength);
        host.log(LOG_LEVEL_PARSED, "Descriptor Type: %d\n", bDescriptorType);
        host.log(LOG_LEVEL_PARSED, "Total Length: %d\n", wTotalLength);
        host.log(LOG_LEVEL_PARSED, "Number of Interfaces: %d\n", bNumInterfaces);
        host.log(LOG_LEVEL_PARSED, "Configuration Value: %d\n", bConfigurationValue);
        host.log(LOG_LEVEL_PARSED, "Configuration Index: %d\n", iConfiguration);
        host.log(LOG_LEVEL_PARSED, "Attributes: 0x%02X\n", bmAttributes);
        host.log(LOG_LEVEL_PARSED, "Max Power: %d (in 2mA units, so %dmA)\n", bMaxPower, bMaxPower * 2);
    }
};


struct LogUnknownDescriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    std::string rawData;

    void appendFromAPI(const usb_standard_desc_t &desc) {
        bLength = desc.bLength;
        bDescriptorType = desc.bDescriptorType;

        std::ostringstream rawDataStream;
        for (int i = 0; i < desc.bLength; ++i) {
            rawDataStream << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << static_cast<int>(desc.val[i]) << " ";
        }
        rawData = rawDataStream.str();
    }

    void logPrint(EspUsbHost &host) const {
        host.log(LOG_LEVEL_PARSED, "**Unknown Descriptor**\n");
        host.log(LOG_LEVEL_PARSED, "Length: %d\n", bLength);
        host.log(LOG_LEVEL_PARSED, "Descriptor Type: %d\n", bDescriptorType);
        host.log(LOG_LEVEL_PARSED, "Raw Data: %s\n", rawData.c_str());
    }
};

struct LogHIDReportDescriptor {
    uint8_t reportId;
    uint8_t buttonSize;
    uint8_t xAxisSize;
    uint8_t yAxisSize;
    uint8_t wheelSize;
    uint8_t buttonStartByte;
    uint8_t xAxisStartByte;
    uint8_t yAxisStartByte;
    uint8_t wheelStartByte;

    void appendFromAPI(const EspUsbHost::HIDReportDescriptor &desc) {
        reportId = desc.reportId;
        buttonSize = desc.buttonSize;
        xAxisSize = desc.xAxisSize;
        yAxisSize = desc.yAxisSize;
        wheelSize = desc.wheelSize;
        buttonStartByte = desc.buttonStartByte;
        xAxisStartByte = desc.xAxisStartByte;
        yAxisStartByte = desc.yAxisStartByte;
        wheelStartByte = desc.wheelStartByte;
    }

    void logPrint(EspUsbHost &host) const {
        host.log(LOG_LEVEL_FIXED, "**HID Report Descriptor**\n");
        host.log(LOG_LEVEL_FIXED, "Report ID: %02X\n", reportId);
        host.log(LOG_LEVEL_FIXED, "Button Bitmap Size: %d\n", buttonSize);
        host.log(LOG_LEVEL_FIXED, "X-Axis Size: %d\n", xAxisSize);
        host.log(LOG_LEVEL_FIXED, "Y-Axis Size: %d\n", yAxisSize);
        host.log(LOG_LEVEL_FIXED, "Wheel Size: %d\n", wheelSize);
        host.log(LOG_LEVEL_FIXED, "Button Start Byte: %d\n", buttonStartByte);
        host.log(LOG_LEVEL_FIXED, "X Axis Start Byte: %d\n", xAxisStartByte);
        host.log(LOG_LEVEL_FIXED, "Y Axis Start Byte: %d\n", yAxisStartByte);
        host.log(LOG_LEVEL_FIXED, "Wheel Start Byte: %d\n", wheelStartByte);
    }
};



    void begin(void);
    static void transmitSerial1(void *arg);
    static void receiveSerial1(void *parameter);
    static void _clientEventCallback(const usb_host_client_event_msg_t *eventMsg, void *arg);
    static void _onReceiveControl(usb_transfer_t *transfer);
    static void monitorInactivity(void *arg);
    static void _onReceive(usb_transfer_t *transfer);
    void get_device_status();
    void suspend_device();
    void resume_device();
    bool serial1Send(const char *format, ...);
    void onConfig(const uint8_t bDescriptorType, const uint8_t *p);
    static String getUsbDescString(const usb_str_desc_t *str_desc);
    esp_err_t submitControl(const uint8_t bmRequestType, const uint8_t bDescriptorIndex, const uint8_t bDescriptorType, const uint16_t wInterfaceNumber, const uint16_t wDescriptorLength);
    void _configCallback(const usb_config_desc_t *config_desc);
   HIDReportDescriptor parseHIDReportDescriptor(uint8_t *data, int length);
    virtual void onReceive(const usb_transfer_t *transfer) {};
    virtual void onGone(const usb_host_client_event_msg_t *eventMsg) {};
    virtual void onMouse(hid_mouse_report_t report, uint8_t last_buttons);
    virtual void onMouseButtons(hid_mouse_report_t report, uint8_t last_buttons);
    virtual void onMouseMove(hid_mouse_report_t report);
    void receiveSerial0(void *command);

    // log
    void logRawBytes(const uint8_t* data, size_t length, const std::string& label);
    /*
    void logHIDReportDescriptor(const HIDReportDescriptor &desc);
    void logDeviceDescriptor(const usb_device_desc_t &dev_desc);
    void logStringDescriptor(const usb_string_descriptor_t &str_desc);
    void logInterfaceDescriptor(const usb_intf_desc_t &intf);
    void logEndpointDescriptor(const usb_ep_desc_t &ep_desc);
    void logInterfaceAssociationDescriptor(const usb_iad_desc_t &iad_desc);
    void logHIDDescriptor(const tusb_hid_descriptor_hid_t &hid_desc);
    void logConfigurationDescriptor(const UsbConfigurationDescriptor &config_desc);
    void logUnknownDescriptor(const usb_standard_desc_t &desc);
*/
   

    void sendDeviceInfo();
    void sendDescriptorDevice();
    void sendEndpointDescriptors();
    void sendInterfaceDescriptors();
    void sendHidDescriptors();
    void sendIADescriptors();
    void sendEndpointData();
    void sendUnknownDescriptors();
    void sendDescriptorconfig();
    void handleIncomingCommands(const String &command);
    void usbLibraryTask(void *arg);
    void usbClientTask(void *arg);

    // Mutex lock (Serial1)
    bool lockMutex(const char *owner);
    void unlockMutex(const char *owner);
};

#endif
