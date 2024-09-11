#include "InitSettings.h"
#include <ArduinoJson.h>

DeviceInfo device_info;
DescriptorDevice descriptor_device;
usb_endpoint_descriptor_t endpoint_descriptors[MAX_ENDPOINT_DESCRIPTORS];
uint8_t endpointCounter;
usb_interface_descriptor_t interface_descriptors[MAX_INTERFACE_DESCRIPTORS];
uint8_t interfaceCounter;
usb_hid_descriptor_t hid_descriptors[MAX_HID_DESCRIPTORS];
uint8_t hidDescriptorCounter;
usb_iad_desc_t descriptor_interface_association;
endpoint_data_t endpoint_data_list[17];
usb_unknown_descriptor_t unknown_descriptors[MAX_UNKNOWN_DESCRIPTORS];
uint8_t unknownDescriptorCounter;
DescriptorConfiguration configuration_descriptor;

const char *stripPrefix(const char *command)
{
    const char *colon = strchr(command, ':');
    if (!colon)
    {
        Serial0.println(F("No colon found in command"));
        return nullptr;
    }

    const char *jsonString = colon + 1;

    while (*jsonString == ' ' || *jsonString == '\n' || *jsonString == '\r')
    {
        jsonString++;
    }

    return jsonString;
}

void printDeviceInfo()
{
    Serial0.print(F("Device Info:\nSpeed: "));
    Serial0.println(device_info.speed);
    Serial0.print(F("Device Address: "));
    Serial0.println(device_info.dev_addr);
    Serial0.print(F("Max Packet Size 0: "));
    Serial0.println(device_info.vMaxPacketSize0);
    Serial0.print(F("Configuration Value: "));
    Serial0.println(device_info.bConfigurationValue);
    Serial0.print(F("Manufacturer: "));
    Serial0.println(device_info.str_desc_manufacturer);
    Serial0.print(F("Product: "));
    Serial0.println(device_info.str_desc_product);
    Serial0.print(F("Serial Number: "));
    Serial0.println(device_info.str_desc_serial_num);
}

void receiveDeviceInfo(const char *command)
{
    const char *jsonString = stripPrefix(command);
    if (!jsonString)
    {
        Serial0.print(F("Invalid command prefix\n"));
        return;
    }

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, jsonString);
    if (error)
    {
        Serial0.print(F("Deserialization failed:\n"));
        Serial0.println(error.c_str());
        Serial0.print(F("Failed JSON string:\n"));
        Serial0.println(jsonString);
        return;
    }

    device_info.speed = doc["speed"];
    device_info.dev_addr = doc["dev_addr"];
    device_info.vMaxPacketSize0 = doc["vMaxPacketSize0"];
    device_info.bConfigurationValue = doc["bConfigurationValue"];
    strlcpy(device_info.str_desc_manufacturer, doc["str_desc_manufacturer"] | "", sizeof(device_info.str_desc_manufacturer));
    strlcpy(device_info.str_desc_product, doc["str_desc_product"] | "", sizeof(device_info.str_desc_product));
    strlcpy(device_info.str_desc_serial_num, doc["str_desc_serial_num"] | "", sizeof(device_info.str_desc_serial_num));
    sendNextCommand();
}

void printDescriptorDevice()
{
    Serial0.print(F("Descriptor Device Info:\n"));
    Serial0.print(F("bLength: "));
    Serial0.println(descriptor_device.bLength);
    Serial0.print(F("bDescriptorType: "));
    Serial0.println(descriptor_device.bDescriptorType);
    Serial0.print(F("bcdUSB: "));
    Serial0.println(descriptor_device.bcdUSB);
    Serial0.print(F("bDeviceClass: "));
    Serial0.println(descriptor_device.bDeviceClass);
    Serial0.print(F("bDeviceSubClass: "));
    Serial0.println(descriptor_device.bDeviceSubClass);
    Serial0.print(F("bDeviceProtocol: "));
    Serial0.println(descriptor_device.bDeviceProtocol);
    Serial0.print(F("bMaxPacketSize0: "));
    Serial0.println(descriptor_device.bMaxPacketSize0);
    Serial0.print(F("idVendor: "));
    Serial0.println(descriptor_device.idVendor);
    Serial0.print(F("idProduct: "));
    Serial0.println(descriptor_device.idProduct);
    Serial0.print(F("bcdDevice: "));
    Serial0.println(descriptor_device.bcdDevice);
    Serial0.print(F("iManufacturer: "));
    Serial0.println(descriptor_device.iManufacturer);
    Serial0.print(F("iProduct: "));
    Serial0.println(descriptor_device.iProduct);
    Serial0.print(F("iSerialNumber: "));
    Serial0.println(descriptor_device.iSerialNumber);
    Serial0.print(F("bNumConfigurations: "));
    Serial0.println(descriptor_device.bNumConfigurations);
}

void receiveDescriptorDevice(const char *command)
{
    const char *jsonString = stripPrefix(command);
    if (!jsonString)
    {
        Serial0.print(F("Invalid command format\n"));
        return;
    }

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, jsonString);
    if (error)
    {
        Serial0.print(F("Deserialization failed:\n"));
        Serial0.println(error.c_str());
        Serial0.print(F("Failed JSON string:\n"));
        Serial0.println(jsonString);
        return;
    }

    descriptor_device.bLength = doc["bLength"];
    descriptor_device.bDescriptorType = doc["bDescriptorType"];
    descriptor_device.bcdUSB = doc["bcdUSB"];
    descriptor_device.bDeviceClass = doc["bDeviceClass"];
    descriptor_device.bDeviceSubClass = doc["bDeviceSubClass"];
    descriptor_device.bDeviceProtocol = doc["bDeviceProtocol"];
    descriptor_device.bMaxPacketSize0 = doc["bMaxPacketSize0"];
    descriptor_device.idVendor = doc["idVendor"];
    descriptor_device.idProduct = doc["idProduct"];
    descriptor_device.bcdDevice = doc["bcdDevice"];
    descriptor_device.iManufacturer = doc["iManufacturer"];
    descriptor_device.iProduct = doc["iProduct"];
    descriptor_device.iSerialNumber = doc["iSerialNumber"];
    descriptor_device.bNumConfigurations = doc["bNumConfigurations"];
    sendNextCommand();
}

void printEndpointDescriptors()
{
    for (int i = 0; i < endpointCounter; i++)
    {
        Serial0.print(F("Endpoint Descriptor "));
        Serial0.println(i);
        Serial0.print(F("bLength: "));
        Serial0.println(endpoint_descriptors[i].bLength);
        Serial0.print(F("bDescriptorType: "));
        Serial0.println(endpoint_descriptors[i].bDescriptorType);
        Serial0.print(F("bEndpointAddress: "));
        Serial0.println(endpoint_descriptors[i].bEndpointAddress);
        Serial0.print(F("endpointID: "));
        Serial0.println(endpoint_descriptors[i].endpointID);
        Serial0.print(F("direction: "));
        Serial0.println(endpoint_descriptors[i].direction);
        Serial0.print(F("bmAttributes: "));
        Serial0.println(endpoint_descriptors[i].bmAttributes);
        Serial0.print(F("attributes: "));
        Serial0.println(endpoint_descriptors[i].attributes);
        Serial0.print(F("wMaxPacketSize: "));
        Serial0.println(endpoint_descriptors[i].wMaxPacketSize);
        Serial0.print(F("bInterval: "));
        Serial0.println(endpoint_descriptors[i].bInterval);
    }
}

void receiveEndpointDescriptors(const char *command)
{
    const char *jsonString = stripPrefix(command);
    if (!jsonString)
    {
        Serial0.print(F("Invalid command prefix\n"));
        return;
    }

    endpointCounter = 0;
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, jsonString);
    if (error)
    {
        Serial0.print(F("Deserialization failed:\n"));
        Serial0.println(error.c_str());
        Serial0.print(F("Failed JSON string:\n"));
        Serial0.println(jsonString);
        return;
    }

    JsonArray array = doc.as<JsonArray>();
    for (JsonObject obj : array)
    {
        if (endpointCounter < MAX_ENDPOINT_DESCRIPTORS)
        {
            endpoint_descriptors[endpointCounter].bLength = obj["bLength"];
            endpoint_descriptors[endpointCounter].bDescriptorType = obj["bDescriptorType"];
            endpoint_descriptors[endpointCounter].bEndpointAddress = obj["bEndpointAddress"];
            endpoint_descriptors[endpointCounter].endpointID = obj["endpointID"];
            endpoint_descriptors[endpointCounter].direction = obj["direction"].as<String>();
            endpoint_descriptors[endpointCounter].bmAttributes = obj["bmAttributes"];
            endpoint_descriptors[endpointCounter].attributes = obj["attributes"].as<String>();
            endpoint_descriptors[endpointCounter].wMaxPacketSize = obj["wMaxPacketSize"];
            endpoint_descriptors[endpointCounter].bInterval = obj["bInterval"];
            endpointCounter++;
        }
    }
    // printEndpointDescriptors();
    sendNextCommand();
}

void printInterfaceDescriptors()
{
    for (int i = 0; i < interfaceCounter; i++)
    {
        Serial0.print(F("Interface Descriptor "));
        Serial0.println(i);
        Serial0.print(F("bLength: "));
        Serial0.println(interface_descriptors[i].bLength);
        Serial0.print(F("bDescriptorType: "));
        Serial0.println(interface_descriptors[i].bDescriptorType);
        Serial0.print(F("bInterfaceNumber: "));
        Serial0.println(interface_descriptors[i].bInterfaceNumber);
        Serial0.print(F("bAlternateSetting: "));
        Serial0.println(interface_descriptors[i].bAlternateSetting);
        Serial0.print(F("bNumEndpoints: "));
        Serial0.println(interface_descriptors[i].bNumEndpoints);
        Serial0.print(F("bInterfaceClass: "));
        Serial0.println(interface_descriptors[i].bInterfaceClass);
        Serial0.print(F("bInterfaceSubClass: "));
        Serial0.println(interface_descriptors[i].bInterfaceSubClass);
        Serial0.print(F("bInterfaceProtocol: "));
        Serial0.println(interface_descriptors[i].bInterfaceProtocol);
        Serial0.print(F("iInterface: "));
        Serial0.println(interface_descriptors[i].iInterface);
    }
}

void receiveInterfaceDescriptors(const char *command)
{
    const char *jsonString = stripPrefix(command);
    if (!jsonString)
    {
        Serial0.print(F("Invalid command prefix\n"));
        return;
    }

    interfaceCounter = 0;
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, jsonString);
    if (error)
    {
        Serial0.print(F("Deserialization failed:\n"));
        Serial0.println(error.c_str());
        Serial0.print(F("Failed JSON string:\n"));
        Serial0.println(jsonString);
        return;
    }

    JsonArray array = doc.as<JsonArray>();
    for (JsonObject obj : array)
    {
        if (interfaceCounter < MAX_INTERFACE_DESCRIPTORS)
        {
            interface_descriptors[interfaceCounter].bLength = obj["bLength"];
            interface_descriptors[interfaceCounter].bDescriptorType = obj["bDescriptorType"];
            interface_descriptors[interfaceCounter].bInterfaceNumber = obj["bInterfaceNumber"];
            interface_descriptors[interfaceCounter].bAlternateSetting = obj["bAlternateSetting"];
            interface_descriptors[interfaceCounter].bNumEndpoints = obj["bNumEndpoints"];
            interface_descriptors[interfaceCounter].bInterfaceClass = obj["bInterfaceClass"];
            interface_descriptors[interfaceCounter].bInterfaceSubClass = obj["bInterfaceSubClass"];
            interface_descriptors[interfaceCounter].bInterfaceProtocol = obj["bInterfaceProtocol"];
            interface_descriptors[interfaceCounter].iInterface = obj["iInterface"];
            interfaceCounter++;
        }
    }
    // printInterfaceDescriptors();
    sendNextCommand();
}

void printHidDescriptors()
{
    for (int i = 0; i < hidDescriptorCounter; i++)
    {
        Serial0.print(F("HID Descriptor "));
        Serial0.println(i);
        Serial0.print(F("bLength: "));
        Serial0.println(hid_descriptors[i].bLength);
        Serial0.print(F("bDescriptorType: "));
        Serial0.println(hid_descriptors[i].bDescriptorType);
        Serial0.print(F("bcdHID: "));
        Serial0.println(hid_descriptors[i].bcdHID);
        Serial0.print(F("bCountryCode: "));
        Serial0.println(hid_descriptors[i].bCountryCode);
        Serial0.print(F("bNumDescriptors: "));
        Serial0.println(hid_descriptors[i].bNumDescriptors);
        Serial0.print(F("bReportType: "));
        Serial0.println(hid_descriptors[i].bReportType);
        Serial0.print(F("wReportLength: "));
        Serial0.println(hid_descriptors[i].wReportLength);
    }
}

void receiveHidDescriptors(const char *command)
{
    const char *jsonString = stripPrefix(command);
    if (!jsonString)
    {
        Serial0.print(F("Invalid command prefix\n"));
        return;
    }

    hidDescriptorCounter = 0;
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, jsonString);
    if (error)
    {
        Serial0.print(F("Deserialization failed:\n"));
        Serial0.println(error.c_str());
        Serial0.print(F("Failed JSON string:\n"));
        Serial0.println(jsonString);
        return;
    }

    JsonArray array = doc.as<JsonArray>();
    for (JsonObject obj : array)
    {
        if (hidDescriptorCounter < MAX_HID_DESCRIPTORS)
        {
            hid_descriptors[hidDescriptorCounter].bLength = obj["bLength"];
            hid_descriptors[hidDescriptorCounter].bDescriptorType = obj["bDescriptorType"];
            hid_descriptors[hidDescriptorCounter].bcdHID = obj["bcdHID"];
            hid_descriptors[hidDescriptorCounter].bCountryCode = obj["bCountryCode"];
            hid_descriptors[hidDescriptorCounter].bNumDescriptors = obj["bNumDescriptors"];
            hid_descriptors[hidDescriptorCounter].bReportType = obj["bReportType"];
            hid_descriptors[hidDescriptorCounter].wReportLength = obj["wReportLength"];
            hidDescriptorCounter++;
        }
    }
    // printHidDescriptors();
    sendNextCommand();
}

void printIADescriptor()
{
    Serial0.print(F("Interface Association Descriptor:\n"));
    Serial0.print(F("bLength: "));
    Serial0.println(descriptor_interface_association.bLength);
    Serial0.print(F("bDescriptorType: "));
    Serial0.println(descriptor_interface_association.bDescriptorType);
    Serial0.print(F("bFirstInterface: "));
    Serial0.println(descriptor_interface_association.bFirstInterface);
    Serial0.print(F("bInterfaceCount: "));
    Serial0.println(descriptor_interface_association.bInterfaceCount);
    Serial0.print(F("bFunctionClass: "));
    Serial0.println(descriptor_interface_association.bFunctionClass);
    Serial0.print(F("bFunctionSubClass: "));
    Serial0.println(descriptor_interface_association.bFunctionSubClass);
    Serial0.print(F("bFunctionProtocol: "));
    Serial0.println(descriptor_interface_association.bFunctionProtocol);
    Serial0.print(F("iFunction: "));
    Serial0.println(descriptor_interface_association.iFunction);
}

void receiveIADescriptors(const char *command)
{
    const char *jsonString = stripPrefix(command);
    if (!jsonString)
    {
        Serial0.print(F("Invalid command prefix\n"));
        return;
    }

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, jsonString);
    if (error)
    {
        Serial0.print(F("Deserialization failed:\n"));
        Serial0.println(error.c_str());
        Serial0.print(F("Failed JSON string:\n"));
        Serial0.println(jsonString);
        return;
    }

    descriptor_interface_association.bLength = doc["bLength"];
    descriptor_interface_association.bDescriptorType = doc["bDescriptorType"];
    descriptor_interface_association.bFirstInterface = doc["bFirstInterface"];
    descriptor_interface_association.bInterfaceCount = doc["bInterfaceCount"];
    descriptor_interface_association.bFunctionClass = doc["bFunctionClass"];
    descriptor_interface_association.bFunctionSubClass = doc["bFunctionSubClass"];
    descriptor_interface_association.bFunctionProtocol = doc["bFunctionProtocol"];
    descriptor_interface_association.iFunction = doc["iFunction"];
    // printIADescriptor();
    sendNextCommand();
}

void printEndpointData()
{
    for (int i = 0; i < 17; i++)
    {
        if (endpoint_data_list[i].bInterfaceClass != 0)
        {
            Serial0.print(F("Endpoint Data "));
            Serial0.println(i);
            Serial0.print(F("bInterfaceNumber: "));
            Serial0.println(endpoint_data_list[i].bInterfaceNumber);
            Serial0.print(F("bInterfaceClass: "));
            Serial0.println(endpoint_data_list[i].bInterfaceClass);
            Serial0.print(F("bInterfaceSubClass: "));
            Serial0.println(endpoint_data_list[i].bInterfaceSubClass);
            Serial0.print(F("bInterfaceProtocol: "));
            Serial0.println(endpoint_data_list[i].bInterfaceProtocol);
            Serial0.print(F("bCountryCode: "));
            Serial0.println(endpoint_data_list[i].bCountryCode);
        }
    }
}

void receiveEndpointData(const char *command)
{
    const char *jsonString = stripPrefix(command);
    if (!jsonString)
    {
        Serial0.print(F("Invalid command prefix\n"));
        return;
    }

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, jsonString);
    if (error)
    {
        Serial0.print(F("Deserialization failed:\n"));
        Serial0.println(error.c_str());
        Serial0.print(F("Failed JSON string:\n"));
        Serial0.println(jsonString);
        return;
    }

    int index = 0;
    JsonArray array = doc.as<JsonArray>();
    for (JsonObject obj : array)
    {
        if (index < 17)
        {
            endpoint_data_list[index].bInterfaceNumber = obj["bInterfaceNumber"];
            endpoint_data_list[index].bInterfaceClass = obj["bInterfaceClass"];
            endpoint_data_list[index].bInterfaceSubClass = obj["bInterfaceSubClass"];
            endpoint_data_list[index].bInterfaceProtocol = obj["bInterfaceProtocol"];
            endpoint_data_list[index].bCountryCode = obj["bCountryCode"];
            index++;
        }
    }
    // printEndpointData();
    sendNextCommand();
}

void printUnknownDescriptors()
{
    for (int i = 0; i < unknownDescriptorCounter; i++)
    {
        Serial0.print(F("Unknown Descriptor "));
        Serial0.println(i);
        Serial0.print(F("bLength: "));
        Serial0.println(unknown_descriptors[i].bLength);
        Serial0.print(F("bDescriptorType: "));
        Serial0.println(unknown_descriptors[i].bDescriptorType);
        Serial0.print(F("data: "));
        Serial0.println(unknown_descriptors[i].data);
    }
}

void receiveUnknownDescriptors(const char *command)
{
    const char *jsonString = stripPrefix(command);
    if (!jsonString)
    {
        Serial0.print(F("Invalid command prefix\n"));
        return;
    }

    unknownDescriptorCounter = 0;
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, jsonString);
    if (error)
    {
        Serial0.print(F("Deserialization failed:\n"));
        Serial0.println(error.c_str());
        Serial0.print(F("Failed JSON string:\n"));
        Serial0.println(jsonString);
        return;
    }

    JsonArray array = doc.as<JsonArray>();
    for (JsonObject obj : array)
    {
        if (unknownDescriptorCounter < MAX_UNKNOWN_DESCRIPTORS)
        {
            unknown_descriptors[unknownDescriptorCounter].bLength = obj["bLength"];
            unknown_descriptors[unknownDescriptorCounter].bDescriptorType = obj["bDescriptorType"];
            unknown_descriptors[unknownDescriptorCounter].data = obj["data"].as<String>();
            unknownDescriptorCounter++;
        }
    }
    // printUnknownDescriptors();
    sendNextCommand();
}

void printDescriptorConfiguration()
{
    Serial0.print(F("Descriptor Configuration:\nLength: "));
    Serial0.println(configuration_descriptor.bLength);
    Serial0.print(F("Descriptor Type: "));
    Serial0.println(configuration_descriptor.bDescriptorType);
    Serial0.print(F("Total Length: "));
    Serial0.println(configuration_descriptor.wTotalLength);
    Serial0.print(F("Number of Interfaces: "));
    Serial0.println(configuration_descriptor.bNumInterfaces);
    Serial0.print(F("Configuration Value: "));
    Serial0.println(configuration_descriptor.bConfigurationValue);
    Serial0.print(F("Configuration String Index: "));
    Serial0.println(configuration_descriptor.iConfiguration);
    Serial0.print(F("Attributes: "));
    Serial0.println(configuration_descriptor.bmAttributes);
    Serial0.print(F("Max Power: "));
    Serial0.println(configuration_descriptor.bMaxPower);
}

void receivedescriptorConfiguration(const char *command)
{
    const char *jsonString = stripPrefix(command);
    if (!jsonString)
    {
        Serial0.print(F("Invalid JSON string\n"));
        return;
    }
    
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, jsonString);
    if (error)
    {
        Serial0.print(F("Deserialization failed:\n"));
        Serial0.println(error.c_str());
        Serial0.print(F("Failed JSON string:\n"));
        Serial0.println(jsonString);
        return;
    }

    configuration_descriptor.bLength = doc["bLength"];
    configuration_descriptor.bDescriptorType = doc["bDescriptorType"];
    configuration_descriptor.wTotalLength = doc["wTotalLength"];
    configuration_descriptor.bNumInterfaces = doc["bNumInterfaces"];
    configuration_descriptor.bConfigurationValue = doc["bConfigurationValue"];
    configuration_descriptor.iConfiguration = doc["iConfiguration"];
    configuration_descriptor.bmAttributes = doc["bmAttributes"];
    configuration_descriptor.bMaxPower = doc["bMaxPower"];

    // printDescriptorConfiguration();
    sendNextCommand();
}

void printParsedDescriptors(const char *command)
{
    Serial0.println("\n**** printDeviceInfo ****");
    printDeviceInfo();
    vTaskDelay(pdMS_TO_TICKS(10));

    Serial0.println("\n**** printDescriptorDevice ****");
    printDescriptorDevice();
    vTaskDelay(pdMS_TO_TICKS(10));

    Serial0.println("\n**** printEndpointDescriptors ****");
    printEndpointDescriptors();
    vTaskDelay(pdMS_TO_TICKS(10));

    Serial0.println("\n**** printInterfaceDescriptors ****");
    printInterfaceDescriptors();
    vTaskDelay(pdMS_TO_TICKS(10));

    Serial0.println("\n**** printHidDescriptors ****");
    printHidDescriptors();
    vTaskDelay(pdMS_TO_TICKS(10));

    Serial0.println("\n**** printIADescriptor ****");
    printIADescriptor();
    vTaskDelay(pdMS_TO_TICKS(10));

    Serial0.println("\n**** printEndpointData ****");
    printEndpointData();

    Serial0.println("\n**** printUnknownDescriptors ****");
    printUnknownDescriptors();

    Serial0.println("\n**** printDescriptorConfiguration ****");
    printDescriptorConfiguration();
    vTaskDelay(pdMS_TO_TICKS(10));
}