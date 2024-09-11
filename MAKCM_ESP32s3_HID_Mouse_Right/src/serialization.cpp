#include "EspUsbHost.h"


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