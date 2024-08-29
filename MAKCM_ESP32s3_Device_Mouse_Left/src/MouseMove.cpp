#include "MouseMove.h"
#include "InitSettings.h"
#include <Arduino.h>
#include <USB.h>
#include <USBHIDMouse.h>
#include "USBSetup.h"
#include <esp_intr_alloc.h>
#include <cstring>

extern TaskHandle_t processCombinedBufferTaskHandle;

volatile bool deviceConnected = false;
#define USB_COMBINED_BUFFER_SIZE 1200  
#define MAX_COMMAND_LENGTH 600

int currentCommandIndex = 0;
bool usbReady = false;
bool processingUsbCommands = false;
bool serial0Locked = true;

char usbCombined[USB_COMBINED_BUFFER_SIZE];
int usbCombinedIndex = 0;
volatile bool usbBufferLocked = false;

char serial1Buffer[MAX_COMMAND_LENGTH];
int serial1BufferIndex = 0;

char serial0Buffer[MAX_COMMAND_LENGTH];
int serial0BufferIndex = 0;

int16_t mouseX = 0;
int16_t mouseY = 0;

char serialBuffer1[BUFFER_SIZE];
int serialBufferIndex1 = 0;

char serialBuffer0[BUFFER_SIZE];
int serialBufferIndex0 = 0;

const unsigned long ledFlashTime = 25; // Set The LED Flash timer in ms


const char *commandQueue[] = {
    "sendDeviceInfo",
    "sendDescriptorDevice",
    "sendEndpointDescriptors",
    "sendInterfaceDescriptors",
    "sendHidDescriptors",
    "sendIADescriptors",
    "sendEndpointData",
    "sendUnknownDescriptors",
    "sendDescriptorconfig"
};

CommandEntry normalCommandTable[] = {
    {"km.move", handleKmMove},
    {"km.moveto", handleKmMoveto},
    {"km.getpos", handleKmGetpos},
    {"km.left(1)", handleKmMouseButtonLeft1},
    {"km.left(0)", handleKmMouseButtonLeft0},
    {"km.right(1)", handleKmMouseButtonRight1},
    {"km.right(0)", handleKmMouseButtonRight0},
    {"km.middle(1)", handleKmMouseButtonMiddle1},
    {"km.middle(0)", handleKmMouseButtonMiddle0},
    {"km.side1(1)", handleKmMouseButtonForward1},
    {"km.side1(0)", handleKmMouseButtonForward0},
    {"km.side2(1)", handleKmMouseButtonBackward1},
    {"km.side2(0)", handleKmMouseButtonBackward0},
    {"km.wheel", handleKmWheel}
};

CommandEntry usbCommandTable[] = {
    {"USB_HELLO", handleUsbHello},
    {"USB_GOODBYE", handleUsbGoodbye},
    {"USB_ISNULL", handleNoDevice},
    {"USB_sendDeviceInfo:", receiveDeviceInfo},
    {"USB_sendDescriptorDevice:", receiveDescriptorDevice},
    {"USB_sendEndpointDescriptors:", receiveEndpointDescriptors},
    {"USB_sendInterfaceDescriptors:", receiveInterfaceDescriptors},
    {"USB_sendHidDescriptors:", receiveHidDescriptors},
    {"USB_sendIADescriptors:", receiveIADescriptors},
    {"USB_sendEndpointData:", receiveEndpointData},
    {"USB_sendUnknownDescriptors:", receiveUnknownDescriptors},
    {"USB_sendDescriptorconfig:", receivedescriptorConfiguration}
};

void moveToThirdBuffer(char* commandBuffer, int commandLength) {
    if (usbCombinedIndex + commandLength + 1 <= USB_COMBINED_BUFFER_SIZE) {
        usbBufferLocked = true;

        strncpy(&usbCombined[usbCombinedIndex], commandBuffer, commandLength);
        usbCombinedIndex += commandLength;
        usbCombined[usbCombinedIndex++] = '\n';

        usbBufferLocked = false;

        if (processCombinedBufferTaskHandle != NULL) {
            xTaskNotifyGive(processCombinedBufferTaskHandle);
        } else {
            Serial.println("Error: processCombinedBufferTaskHandle is NULL");
        }
    } else {
        Serial.println("Combined buffer overflow detected, clearing buffer.");
        usbCombinedIndex = 0;
    }
}

void processCombinedBuffer() {
    if (usbCombinedIndex > 0) {
        usbBufferLocked = true;

        int startIndex = 0;
        for (int i = 0; i < usbCombinedIndex; i++) {
            if (usbCombined[i] == '\n') {
                usbCombined[i] = '\0';
                if (startIndex < i) {
                    processSerialrx(&usbCombined[startIndex], i - startIndex);
                }
                startIndex = i + 1;
            }
        }

        if (startIndex < usbCombinedIndex) {
            memmove(usbCombined, &usbCombined[startIndex], usbCombinedIndex - startIndex);
            usbCombinedIndex -= startIndex;
        } else {
            usbCombinedIndex = 0;
        }

        usbBufferLocked = false;  
    }
}

void serial1RX() {
    while (Serial1.available() > 0) {
        char byte = Serial1.read();

        if (byte == '\r') {
            continue;
        }

        if (byte == '\n') {
            if (serial1BufferIndex > 0) {
                serial1Buffer[serial1BufferIndex] = '\0';
                moveToThirdBuffer(serial1Buffer, serial1BufferIndex);
                serial1BufferIndex = 0;
            }
        } else if (serial1BufferIndex < MAX_COMMAND_LENGTH - 1) {
            serial1Buffer[serial1BufferIndex++] = byte;
        } else {
            serial1BufferIndex = 0;
            Serial.println("Serial1 buffer overflow detected, clearing buffer.");
        }
    }
}

void serial0RX() {
    while (Serial0.available() > 0) {
        char byte = Serial0.read();

        if (byte == '\r') {
            continue;
        }

        if (byte == '\n') {
            if (serial0BufferIndex > 0) {
                serial0Buffer[serial0BufferIndex] = '\0';
                moveToThirdBuffer(serial0Buffer, serial0BufferIndex);
                serial0BufferIndex = 0;

                // Flash LED when a complete command is received
                flashLED();
            }
        } else if (serial0BufferIndex < MAX_COMMAND_LENGTH - 1) {
            serial0Buffer[serial0BufferIndex++] = byte;
        } else {
            serial0BufferIndex = 0;
            Serial.println("Serial0 buffer overflow detected, clearing buffer.");
        }
    }
}


void ledFlashTask(void *parameter) {
    digitalWrite(9, HIGH);
    vTaskDelay(ledFlashTime / portTICK_PERIOD_MS);
    digitalWrite(9, LOW);
    vTaskDelay(ledFlashTime / portTICK_PERIOD_MS);
    bool *taskRunning = (bool *)parameter; 
    *taskRunning = false;
    vTaskDelete(NULL);
}

void flashLED() {
    static bool ledFlashTaskRunning = false;
    if (!ledFlashTaskRunning) {
        ledFlashTaskRunning = true;
        xTaskCreate(ledFlashTask, "LED Flash Task", 1024, &ledFlashTaskRunning, 1, NULL);
    }
}

void handleNoDevice(const char *command) {
    deviceConnected = false;
}

void handleUsbHello(const char *command) {
    deviceConnected = true;
    usbReady = true;
    processingUsbCommands = true;
    currentCommandIndex = 0;
    sendNextCommand();
}

void handleUsbGoodbye(const char *command) {
    Serial0.println("USB Device disconnected. Restarting!");

    handleMove(0, 0);
    handleMouseButton(MOUSE_BUTTON_LEFT, false);
    handleMouseButton(MOUSE_BUTTON_RIGHT, false);
    handleMouseButton(MOUSE_BUTTON_MIDDLE, false);
    handleMouseButton(MOUSE_BUTTON_FORWARD, false);
    handleMouseButton(MOUSE_BUTTON_BACKWARD, false);
    handleMouseWheel(0);
    vTaskDelay(100);
    ESP.restart();
}

void sendNextCommand() {
    if (!processingUsbCommands || currentCommandIndex >= sizeof(commandQueue) / sizeof(commandQueue[0])) {
        return;
    }

    const char *command = commandQueue[currentCommandIndex];
    Serial1.println(command);
    currentCommandIndex++;

    if (currentCommandIndex >= sizeof(commandQueue) / sizeof(commandQueue[0])) {
        usbReady = false;
        processingUsbCommands = false;
        InitUSB();
        vTaskDelay(700);
        serial0Locked = false;
        Serial1.println("USB_INIT");
    }
}

void processSerialrx(const char *command, int length) {
    for (const auto &entry : usbCommandTable) {
        if (strncmp(command, entry.command, strlen(entry.command)) == 0) {
            entry.handler(command);
            return;
        }
    }

    if (!processingUsbCommands) {
        for (const auto &entry : normalCommandTable) {
            if (strncmp(command, entry.command, strlen(entry.command)) == 0) {
                entry.handler(command);
                return;
            }
        }
    }

    handleDebugcommand(command);
}

void handleDebugcommand(const char *command) {
    Serial0.print("DEBUG:Right ");
    Serial0.print(command);
}

void handleKmMove(const char *command) {
    int x, y;
    sscanf(command + strlen("km.move") + 1, "%d,%d", &x, &y);
    handleMove(x, y);
}

void handleKmMoveto(const char *command) {
    int x, y;
    sscanf(command + strlen("km.moveto") + 1, "%d,%d", &x, &y);
    handleMoveto(x, y);
}

void handleKmGetpos(const char *command) {
    handleGetPos();
}

void handleKmMouseButtonLeft1(const char *command) { handleMouseButton(MOUSE_BUTTON_LEFT, true); }
void handleKmMouseButtonLeft0(const char *command) { handleMouseButton(MOUSE_BUTTON_LEFT, false); }
void handleKmMouseButtonRight1(const char *command) { handleMouseButton(MOUSE_BUTTON_RIGHT, true); }
void handleKmMouseButtonRight0(const char *command) { handleMouseButton(MOUSE_BUTTON_RIGHT, false); }
void handleKmMouseButtonMiddle1(const char *command) { handleMouseButton(MOUSE_BUTTON_MIDDLE, true); }
void handleKmMouseButtonMiddle0(const char *command) { handleMouseButton(MOUSE_BUTTON_MIDDLE, false); }
void handleKmMouseButtonForward1(const char *command) { handleMouseButton(MOUSE_BUTTON_FORWARD, true); }
void handleKmMouseButtonForward0(const char *command) { handleMouseButton(MOUSE_BUTTON_FORWARD, false); }
void handleKmMouseButtonBackward1(const char *command) { handleMouseButton(MOUSE_BUTTON_BACKWARD, true); }
void handleKmMouseButtonBackward0(const char *command) { handleMouseButton(MOUSE_BUTTON_BACKWARD, false); }

void handleKmWheel(const char *command) {
    int wheelMovement;
    sscanf(command + strlen("km.wheel") + 1, "%d", &wheelMovement);
    handleMouseWheel(wheelMovement);
}

void handleMove(int x, int y) {
    Mouse.move(x, y);
    mouseX += x;
    mouseY += y;
}

void handleMoveto(int x, int y) {
    Mouse.move(x - mouseX, y - mouseY);
    mouseX = x;
    mouseY = y;
}

void handleMouseButton(uint8_t button, bool press) {
    if (press)
        Mouse.press(button);
    else
        Mouse.release(button);
}

void handleMouseWheel(int wheelMovement) {
    Mouse.move(0, 0, wheelMovement);
}

void handleGetPos() {
    Serial0.println("km.pos(" + String(mouseX) + "," + String(mouseY) + ")");
}
