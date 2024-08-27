#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// Constants for maximum descriptors
#define MAX_ENDPOINT_DESCRIPTORS 10
#define MAX_INTERFACE_DESCRIPTORS 10
#define MAX_HID_DESCRIPTORS 10
#define MAX_UNKNOWN_DESCRIPTORS 10

// Function declarations
extern void sendNextCommand();

// Struct definitions
struct DeviceInfo {
    uint8_t speed;
    uint8_t dev_addr;
    uint8_t vMaxPacketSize0;
    uint8_t bConfigurationValue;
    char str_desc_manufacturer[64];
    char str_desc_product[64];
    char str_desc_serial_num[64];
};

struct DescriptorDevice {
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
};

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
};

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
};

struct usb_hid_descriptor_t {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t bcdHID;
    uint8_t bCountryCode;
    uint8_t bNumDescriptors;
    uint8_t bReportType;
    uint16_t wReportLength;
};

struct usb_iad_desc_t {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bFirstInterface;
    uint8_t bInterfaceCount;
    uint8_t bFunctionClass;
    uint8_t bFunctionSubClass;
    uint8_t bFunctionProtocol;
    uint8_t iFunction;
};

struct endpoint_data_t {
    uint8_t bInterfaceNumber;
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t bCountryCode;
};

struct usb_unknown_descriptor_t {
    uint8_t bLength;
    uint8_t bDescriptorType;
    String data;
};

struct DescriptorConfiguration {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t wTotalLength;
    uint8_t bNumInterfaces;
    uint8_t bConfigurationValue;
    uint8_t iConfiguration;
    uint8_t bmAttributes;
    uint8_t bMaxPower;
};

// External variable declarations
extern DeviceInfo device_info;
extern DescriptorDevice descriptor_device;
extern usb_endpoint_descriptor_t endpoint_descriptors[MAX_ENDPOINT_DESCRIPTORS];
extern uint8_t endpointCounter;
extern usb_interface_descriptor_t interface_descriptors[MAX_INTERFACE_DESCRIPTORS];
extern uint8_t interfaceCounter;
extern usb_hid_descriptor_t hid_descriptors[MAX_HID_DESCRIPTORS];
extern uint8_t hidDescriptorCounter;
extern usb_iad_desc_t descriptor_interface_association;
extern endpoint_data_t endpoint_data_list[17];
extern usb_unknown_descriptor_t unknown_descriptors[MAX_UNKNOWN_DESCRIPTORS];
extern uint8_t unknownDescriptorCounter;
extern DescriptorConfiguration configuration_descriptor;

// Function prototypes
void printDeviceInfo();
void printDescriptorDevice();
void printEndpointDescriptors();
void printInterfaceDescriptors();
void printHidDescriptors();
void printIADescriptor();
void printEndpointData();
void printUnknownDescriptors();
void printDescriptorConfiguration();

void receiveDeviceInfo(const char *jsonString);
void receiveDescriptorDevice(const char *jsonString);
void receiveEndpointDescriptors(const char *jsonString);
void receiveInterfaceDescriptors(const char *jsonString);
void receiveHidDescriptors(const char *jsonString);
void receiveIADescriptors(const char *jsonString);
void receiveEndpointData(const char *jsonString);
void receiveUnknownDescriptors(const char *jsonString);
void receiveDescriptorConfiguration(const char *jsonString);

