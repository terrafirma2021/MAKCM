#pragma once

#include "InitSettings.h"
#include <Arduino.h>
#include <USB.h>
#include <USBHIDMouse.h>
#include "USBSetup.h"
#include <esp_intr_alloc.h>
#include <cstring>
#include <atomic>

// Extern variables
extern USBHIDMouse Mouse;
extern TaskHandle_t mouseMoveTaskHandle;
extern TaskHandle_t ledFlashTaskHandle;
extern const char *commandQueue[];
extern int currentCommandIndex;
extern bool usbReady;


// Mouse position tracking
extern int16_t mouseX;
extern int16_t mouseY;

// Buffer lengths
#define MAX_KM_MOVE_COMMAND_LENGTH 20
#define MAX_SERIAL0_COMMAND_LENGTH 100
#define MAX_SERIAL1_COMMAND_LENGTH 600

// Command buffers
extern char serial0Buffer[MAX_SERIAL0_COMMAND_LENGTH];
extern char serial1Buffer[MAX_SERIAL1_COMMAND_LENGTH];

// Atomic flags for button states
extern std::atomic<bool> isLeftButtonPressed;
extern std::atomic<bool> isRightButtonPressed;
extern std::atomic<bool> isMiddleButtonPressed;
extern std::atomic<bool> isForwardButtonPressed;
extern std::atomic<bool> isBackwardButtonPressed;
extern std::atomic<bool> serial0Locked;

// Function declarations
void handleKmMoveCommand(const char *command);
void handleDebugcommand(const char *command);
void handleMove(int x, int y);
void handleMoveto(int x, int y);
void handleMouseButton(uint8_t button, bool press);
void handleMouseWheel(int wheelMovement);
void handleGetPos();
void serial1RX();
void serial0RX();
void notifyLedFlashTask();

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
void handleDebug(const char* command);
void handleSerial0Speed(const char* command);
void handleEspLog(const char* command);
void sendNextCommand();
void processCommand(const char *command);

// Extern functions for JSON data handling
extern void receiveDeviceInfo(const char *jsonString);
extern void receiveDescriptorDevice(const char *jsonString);
extern void receiveEndpointDescriptors(const char *jsonString);
extern void receiveInterfaceDescriptors(const char *jsonString);
extern void receiveHidDescriptors(const char *jsonString);
extern void receiveIADescriptors(const char *jsonString);
extern void receiveEndpointData(const char *jsonString);
extern void receiveUnknownDescriptors(const char *jsonString);
extern void receivedescriptorConfiguration(const char *jsonString);

// Command table structure
struct CommandEntry {
    const char *command;
    void (*handler)(const char *);
};
