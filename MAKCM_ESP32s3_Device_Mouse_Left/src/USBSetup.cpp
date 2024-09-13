#include "USBSetup.h"
#include <USBHIDMouse.h>
#include <USB.h>
#include "tusb.h"

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
extern volatile bool deviceConnected;
extern bool usbIsDebug;

USBHIDMouse Mouse;
extern ESPUSB USB;

/*

// prep migration
tud_descriptor_device_cb, tud_descriptor_configuration_cb, tud_descriptor_string_cb 
*/

uint16_t const* tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    static uint16_t desc[32];
    const char* str;
    uint8_t chr_count;

    if (index == 0) {
        // Return language ID string descriptor (English) for now!
        desc[1] = 0x0409;
        desc[0] = (TUSB_DESC_STRING << 8) | (2 + 2);
        return desc;
    }


    switch (index) {
        case 1:
            str = device_info.str_desc_manufacturer;
            break;
        case 2:
            str = device_info.str_desc_product;
            break;
        case 3:
            str = device_info.str_desc_serial_num;
            break;
        default:
            return NULL; // TO DO, parse further strings
    }

    chr_count = strlen(str);
    for (uint8_t i = 0; i < chr_count; i++) {
        desc[1 + i] = str[i];
    }

    desc[0] = (TUSB_DESC_STRING << 8) | (2 * chr_count + 2);

    return desc;
}

void requestUSBDescriptors() {
    vTaskDelay(200);
    if (deviceConnected) {
        return;
    }
    Serial1.println("READY");
}

void InitUSB() {

    USB.usbVersion(descriptor_device.bcdUSB);
    USB.firmwareVersion(descriptor_device.bcdDevice);
    USB.usbPower(configuration_descriptor.bMaxPower);
    USB.usbAttributes(configuration_descriptor.bmAttributes);
    USB.usbClass(descriptor_device.bDeviceClass);
    USB.usbSubClass(descriptor_device.bDeviceSubClass);
    USB.usbProtocol(descriptor_device.bDeviceProtocol);

    Mouse.begin();
    USB.begin();
}
