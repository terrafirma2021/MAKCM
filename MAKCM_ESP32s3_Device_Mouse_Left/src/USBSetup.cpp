#include "USBSetup.h"
#include <USB.h>
#include "tusb.h"
#include "BridgeTransport.h"
#include "PassthroughDevice.h"

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

extern ESPUSB USB;

/*

// prep migration
tud_descriptor_device_cb, tud_descriptor_configuration_cb, tud_descriptor_string_cb 
*/

extern "C" uint8_t const *tud_descriptor_device_cb(void) {
    return Passthrough.profileReady() ? Passthrough.deviceDescriptor() : nullptr;
}

extern "C" uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    return index == 0 && Passthrough.profileReady()
        ? Passthrough.configurationDescriptor()
        : nullptr;
}

extern "C" uint16_t const *tud_descriptor_string_cb(uint8_t index,
                                                     uint16_t langid) {
    return Passthrough.stringDescriptor(index, langid);
}

void requestUSBDescriptors() {
    vTaskDelay(200);
}

void InitUSB() {
    static bool initialized = false;
    if (initialized) {
        Passthrough.reconnectUsb();
        return;
    }
    initialized = true;

    if (!Passthrough.profileReady() || !Passthrough.beginUsb()) {
        initialized = false;
        Serial0.println("USB passthrough profile is incomplete");
        return;
    }

    USB.VID(descriptor_device.idVendor);
    USB.PID(descriptor_device.idProduct);
    USB.manufacturerName(device_info.str_desc_manufacturer);
    USB.productName(device_info.str_desc_product);
    USB.serialNumber(device_info.str_desc_serial_num);
    USB.usbVersion(descriptor_device.bcdUSB);
    USB.firmwareVersion(descriptor_device.bcdDevice);
    USB.usbPower(Passthrough.profile().configPowerMa);
    USB.usbAttributes(configuration_descriptor.bmAttributes);
    USB.usbClass(descriptor_device.bDeviceClass);
    USB.usbSubClass(descriptor_device.bDeviceSubClass);
    USB.usbProtocol(descriptor_device.bDeviceProtocol);

    USB.begin();
}
