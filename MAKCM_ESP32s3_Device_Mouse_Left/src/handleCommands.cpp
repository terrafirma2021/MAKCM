#include "handleCommands.h"
#include "InitSettings.h"
#include "tasks.h"
#include <Arduino.h>
#include <USB.h>
#include <USBHIDMouse.h>
#include "USBSetup.h"
#include <esp_intr_alloc.h>
#include <cstring>
#include <atomic>
#include <mutex>

// Atomic variables for mouse movement and button states
std::atomic<int> moveX(0);
std::atomic<int> moveY(0);
std::atomic<bool> isLeftButtonPressed(false);
std::atomic<bool> isRightButtonPressed(false);
std::atomic<bool> isMiddleButtonPressed(false);
std::atomic<bool> isForwardButtonPressed(false);
std::atomic<bool> isBackwardButtonPressed(false);
std::atomic<bool> serial0Locked(true);

// Task handles
extern TaskHandle_t mouseMoveTaskHandle;
extern TaskHandle_t ledFlashTaskHandle;

std::mutex commandMutex;

volatile bool deviceConnected = false;
bool usbReady = false;
bool processingUsbCommands = false;
std::atomic<bool> kmMoveCom(false);

int currentCommandIndex = 0;
char serial0Buffer[MAX_SERIAL0_COMMAND_LENGTH];
char serial1Buffer[MAX_SERIAL1_COMMAND_LENGTH];
int serial0BufferIndex = 0;
int serial1BufferIndex = 0;

int16_t mouseX = 0;
int16_t mouseY = 0;

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

// Command tables
CommandEntry serial0CommandTable[] = {
    {"DEBUG_", handleDebug},
    {"SERIAL_", handleSerial0Speed}
};

CommandEntry debugCommandTable[] = {
    {"ESPLOG_", handleEspLog}
};

CommandEntry normalCommandTable[] = {
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

void processCommand(const char *command);

void serial0RX() {
    while (Serial0.available() > 0) {
        char byte = Serial0.read();
        if (byte == '\r') {
            continue;
        }
        if (byte == '\n') {
            if (serial0BufferIndex > 0) {
                serial0Buffer[serial0BufferIndex] = '\0';
                if (serial0BufferIndex <= MAX_KM_MOVE_COMMAND_LENGTH && strncmp(serial0Buffer, "km.move", 7) == 0) {
                    kmMoveCom = true;
                    handleKmMoveCommand(serial0Buffer);
                } else {
                    processCommand(serial0Buffer);
                }
                serial0BufferIndex = 0;
                notifyLedFlashTask();
            }
        } else if (serial0BufferIndex < MAX_SERIAL0_COMMAND_LENGTH - 1) {
            serial0Buffer[serial0BufferIndex++] = byte;
        } else {
            serial0BufferIndex = 0;
            Serial.println("Serial0 buffer overflow detected, clearing buffer.");
        }
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
                if (serial1BufferIndex <= MAX_KM_MOVE_COMMAND_LENGTH && strncmp(serial1Buffer, "km.move", 7) == 0) {
                    if (!kmMoveCom) {
                        handleKmMoveCommand(serial1Buffer);
                    }
                } else {
                    processCommand(serial1Buffer);
                }
                serial1BufferIndex = 0;
            }
        } else if (serial1BufferIndex < MAX_SERIAL1_COMMAND_LENGTH - 1) {
            serial1Buffer[serial1BufferIndex++] = byte;
        } else {
            serial1BufferIndex = 0;
            Serial.println("Serial1 buffer overflow detected, clearing buffer.");
        }
    }
}

void handleKmMoveCommand(const char *command) {
    int x, y;
    sscanf(command + strlen("km.move") + 1, "%d,%d", &x, &y);
    {
        std::lock_guard<std::mutex> lock(commandMutex);
        moveX = x;
        moveY = y;
    }
    if (mouseMoveTaskHandle != NULL) {
        xTaskNotifyGive(mouseMoveTaskHandle);
    }
    kmMoveCom = false;
}

void ledFlashTask(void *parameter) {
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        digitalWrite(9, HIGH);
        vTaskDelay(ledFlashTime / portTICK_PERIOD_MS);
        digitalWrite(9, LOW);
        vTaskDelay(ledFlashTime / portTICK_PERIOD_MS);
    }
}

void notifyLedFlashTask() {
    xTaskNotifyGive(ledFlashTaskHandle);
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

void processCommand(const char *command) {
    for (const auto &entry : debugCommandTable) {
        if (strncmp(command, entry.command, strlen(entry.command)) == 0) {
            entry.handler(command);
            return;
        }
    }
    for (const auto &entry : serial0CommandTable) {
        if (strncmp(command, entry.command, strlen(entry.command)) == 0) {
            entry.handler(command);
            return;
        }
    }
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

void handleEspLog(const char *command) {
    const char *message = command + strlen("ESPLOG_");
    if (strlen(message) > 0) {
        Serial0.println(message);
    } else {
        Serial0.println("ESPLOG_ command received, but no message to log.");
    }
}

void handleSerial0Speed(const char *command) {
    int speed;
    if (sscanf(command + strlen("SERIAL_"), "%d", &speed) == 1) {
        if (speed >= 115200 && speed <= 5000000) {
            Serial0.print("Setting Serial0 speed to: ");
            Serial0.println(speed);
            Serial0.end();
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            Serial0.begin(speed);
            Serial0.onReceive(serial0ISR);
            Serial0.println("Serial0 speed change successful.");
        } else {
            Serial0.println("Speed was out of bounds. Min: 115200, Max: 5000000.");
        }
    } else {
        Serial0.println("Invalid SERIAL command. Expected format: SERIAL_<speed>");
    }
}

void handleDebug(const char *command) {
    int debugLevel;
    if (strcmp(command, "DEBUG_ON") == 0) {
        Serial1.println("DEBUG_ON");
    } else if (strcmp(command, "DEBUG_OFF") == 0) {
        Serial1.println("DEBUG_OFF");
    } else if (sscanf(command + strlen("DEBUG_"), "%d", &debugLevel) == 1) {
        Serial1.println(command);
    } else {
        Serial0.println("Invalid DEBUG command.");
    }
}

void handleDebugcommand(const char *command) {
    Serial0.print("Right MCU : ");
    Serial0.print(command);
}

void handleNoDevice(const char *command)
{
    deviceConnected = false;
}

void mouseMoveTask(void *pvParameters) {
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        int x, y;
        {
            std::lock_guard<std::mutex> lock(commandMutex);
            x = moveX;
            y = moveY;
        }
        handleMove(x, y);
    }
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

void handleKmMouseButtonLeft1(const char *command) {
    if (!isLeftButtonPressed.exchange(true)) {
        handleMouseButton(MOUSE_BUTTON_LEFT, true);
    }
}

void handleKmMouseButtonLeft0(const char *command) {
    if (isLeftButtonPressed.exchange(false)) {
        handleMouseButton(MOUSE_BUTTON_LEFT, false);
    }
}

void handleKmMouseButtonRight1(const char *command) {
    if (!isRightButtonPressed.exchange(true)) {
        handleMouseButton(MOUSE_BUTTON_RIGHT, true);
    }
}

void handleKmMouseButtonRight0(const char *command) {
    if (isRightButtonPressed.exchange(false)) {
        handleMouseButton(MOUSE_BUTTON_RIGHT, false);
    }
}

void handleKmMouseButtonMiddle1(const char *command) {
    if (!isMiddleButtonPressed.exchange(true)) {
        handleMouseButton(MOUSE_BUTTON_MIDDLE, true);
    }
}

void handleKmMouseButtonMiddle0(const char *command) {
    if (isMiddleButtonPressed.exchange(false)) {
        handleMouseButton(MOUSE_BUTTON_MIDDLE, false);
    }
}

void handleKmMouseButtonForward1(const char *command) {
    if (!isForwardButtonPressed.exchange(true)) {
        handleMouseButton(MOUSE_BUTTON_FORWARD, true);
    }
}

void handleKmMouseButtonForward0(const char *command) {
    if (isForwardButtonPressed.exchange(false)) {
        handleMouseButton(MOUSE_BUTTON_FORWARD, false);
    }
}

void handleKmMouseButtonBackward1(const char *command) {
    if (!isBackwardButtonPressed.exchange(true)) {
        handleMouseButton(MOUSE_BUTTON_BACKWARD, true);
    }
}

void handleKmMouseButtonBackward0(const char *command) {
    if (isBackwardButtonPressed.exchange(false)) {
        handleMouseButton(MOUSE_BUTTON_BACKWARD, false);
    }
}

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
