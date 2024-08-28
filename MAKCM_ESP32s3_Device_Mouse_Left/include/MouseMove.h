#pragma once

#include "InitSettings.h"
#include <Arduino.h>
#include <USB.h>
#include <USBHIDMouse.h>
#include "USBSetup.h"
#include <esp_intr_alloc.h>
#include <cstring>

// Extern variables
extern USBHIDMouse Mouse;

extern const char *commandQueue[];
extern int currentCommandIndex;
extern bool usbReady;

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

extern int16_t mouseX;
extern int16_t mouseY;

#define BUFFER_SIZE 1024
extern char serialBuffer1[BUFFER_SIZE];
extern int serialBufferIndex1;

extern char serialBuffer0[BUFFER_SIZE];
extern int serialBufferIndex0;

// Function prototypes

void processSerialrx(const char *command, int length);
void handleDebugcommand(const char *command);

void handleMove(int x, int y);
void handleMoveto(int x, int y);
void handleMouseButton(uint8_t button, bool press);
void handleMouseWheel(int wheelMovement);
void handleGetPos();

void serial1RX();
void serial0RX();
void flashLED();

void handleKmMove(const char *command);
void handleKmMoveto(const char *command);
void handleKmGetpos(const char *command);
void handleKmMouseButtonLeft1(const char *command);
void handleKmMouseButtonLeft0(const char *command);
void handleKmMouseButtonRight1(const char *command);
void handleKmMouseButtonRight0(const char *command);
void handleKmMouseButtonMiddle1(const char *command);
void handleKmMouseButtonMiddle0(const char *command);
void handleKmMouseButtonForward1(const char *command);
void handleKmMouseButtonForward0(const char *command);
void handleKmMouseButtonBackward1(const char *command);
void handleKmMouseButtonBackward0(const char *command);
void handleKmWheel(const char *command);

void handleUsbHello(const char *command);
void handleUsbGoodbye(const char *command);
void handleNoDevice(const char *command);

void sendNextCommand();

extern void receiveDeviceInfo(const char *jsonString);
extern void receiveDescriptorDevice(const char *jsonString);
extern void receiveEndpointDescriptors(const char *jsonString);
extern void receiveInterfaceDescriptors(const char *jsonString);
extern void receiveHidDescriptors(const char *jsonString);
extern void receiveIADescriptors(const char *jsonString);
extern void receiveEndpointData(const char *jsonString);
extern void receiveUnknownDescriptors(const char *jsonString);
extern void receivedescriptorConfiguration(const char *jsonString);

struct CommandEntry {
    const char *command;
    void (*handler)(const char *);
};

extern CommandEntry commandTable[];

