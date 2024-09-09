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
#define LOG_LEVEL_OFF   0
#define LOG_LEVEL_INFO  1
#define LOG_LEVEL_WARN  2
#define LOG_LEVEL_ERROR 3
#define LOG_LEVEL_DEBUG 4
#define LOG_LEVEL_PARSED 5

#define LOG_QUEUE_SIZE 20
#define LOG_MESSAGE_SIZE 620

static void usb_lib_task(void *arg);
static void usb_client_task(void *arg);
void ledFlashTask(void *parameter);
void processLogQueue(void* parameter);



class EspUsbHost
{
public:
    // Debug and Log
    bool debugModeActive = false;
    void log(int level, const char *format, ...);
    static int current_log_level;


    bool isReady = false;
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

    void begin(void);
    static void tx_esp(void *arg);
    static void rx_esp(void *parameter);
    static void _clientEventCallback(const usb_host_client_event_msg_t *eventMsg, void *arg);
    static void _onReceiveControl(usb_transfer_t *transfer);
    static void monitor_inactivity_task(void *arg);
    static void _onReceive(usb_transfer_t *transfer);
    void get_device_status();
    void suspend_device();
    void resume_device();
    bool tx_Que(const char *format, ...);
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
    void rx_esp_serial0(void *command);

    // log
    void logRawBytes(const uint8_t* data, size_t length, const std::string& label);
    void logHIDReportDescriptor(const HIDReportDescriptor &desc);
    void logDeviceDescriptor(const usb_device_desc_t &dev_desc);
    void logStringDescriptor(const usb_string_descriptor_t &str_desc);
    void logInterfaceDescriptor(const usb_intf_desc_t &intf);
    void logEndpointDescriptor(const usb_ep_desc_t &ep_desc);
    void logInterfaceAssociationDescriptor(const usb_iad_desc_t &iad_desc);
    void logHIDDescriptor(const tusb_hid_descriptor_hid_t &hid_desc);
    void logConfigurationDescriptor(const UsbConfigurationDescriptor &config_desc);
    void logUnknownDescriptor(const usb_standard_desc_t &desc);

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
};

#endif
