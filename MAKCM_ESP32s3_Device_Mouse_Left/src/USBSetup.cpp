#include "USBSetup.h"    
#include <USBHIDMouse.h> 
#include <USB.h>         


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

USBHIDMouse Mouse;


void requestUSBDescriptors()
{
    vTaskDelay(200); 
    if (deviceConnected)
    {
        return;
    }
    Serial1.println("READY");
}


void InitUSB()
{
    USB.VID(descriptor_device.idVendor);
    USB.PID(descriptor_device.idProduct);
    USB.usbVersion(descriptor_device.bcdUSB);
    USB.firmwareVersion(descriptor_device.bcdDevice);
    USB.usbPower(configuration_descriptor.bMaxPower);
    USB.usbAttributes(configuration_descriptor.bmAttributes);
    USB.usbClass(descriptor_device.bDeviceClass);
    USB.usbSubClass(descriptor_device.bDeviceSubClass);
    USB.usbProtocol(descriptor_device.bDeviceProtocol);
    USB.productName(device_info.str_desc_product);
    USB.manufacturerName(device_info.str_desc_manufacturer);
    USB.serialNumber(device_info.str_desc_serial_num);

    Mouse.begin();
    USB.begin();
}
