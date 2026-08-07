#include "handleCommands.h"
#include "InitSettings.h"
#include "tasks.h"
#include <Arduino.h>
#include <USB.h>
#include "USBSetup.h"
#include "BridgeTransport.h"
#include "InputMonitor.h"
#include "MakcuWireTrace.h"
#include "PassthroughDevice.h"
#include <esp_intr_alloc.h>
#include <cstring>
#include <atomic>
#include <RingBuf.h>
#include <freertos/semphr.h>

namespace {
SemaphoreHandle_t mouseCommandMutex = nullptr;

bool lockMouseCommands() {
    return mouseCommandMutex == nullptr ||
        xSemaphoreTake(mouseCommandMutex, portMAX_DELAY) == pdTRUE;
}

void unlockMouseCommands() {
    if (mouseCommandMutex != nullptr) xSemaphoreGive(mouseCommandMutex);
}
} // namespace

std::atomic<bool> serial0Locked(true);

// Task handles
extern TaskHandle_t ledFlashTaskHandle;

volatile bool deviceConnected = false;
bool usbReady = false;
bool processingUsbCommands = false;



// Create the ring buffers
void processRingBufferCommand(RingBuf<char, 620> &buffer);
RingBuf<char, 620> serial0RingBuffer;
RingBuf<char, 620> serial1RingBuffer;
int currentCommandIndex = 0;

int64_t mouseX = 0;
int64_t mouseY = 0;

void initializeCommandState() {
    if (mouseCommandMutex == nullptr) {
        mouseCommandMutex = xSemaphoreCreateMutex();
    }
}

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
    "sendDescriptorconfig",
    "sendPassthroughProfile"
};

// Command tables
CommandEntry serial0CommandTable[] = {
    {"DEBUG_", handleDebug},
    {"SERIAL_", handleSerial0Speed}
};

CommandEntry debugCommandTable[] = {
    {"ESPLOG_", handleEspLog},
    {"PRINT_Parsed_Descriptors", printParsedDescriptors},
    {"HID_Descriptors", [](const char* arg) { sendBridgeText(arg, false); }}
};

CommandEntry normalCommandTable[] = {
    {"BRIDGE_TRACE_START", [](const char *) {
        MakcuWireTrace::start(false);
        Serial0.println("BRIDGE_TRACE_START ok");
    }},
    {"BRIDGE_TRACE_STOP", [](const char *) {
        MakcuWireTrace::stop();
        MakcuWireTrace::printStatus(Serial0);
    }},
    {"BRIDGE_TRACE_STATUS", [](const char *) {
        MakcuWireTrace::printStatus(Serial0);
    }},
    {"BRIDGE_TRACE_DUMP", [](const char *) {
        MakcuWireTrace::dumpHex(Serial0);
    }},
    {"BRIDGE_STATUS", [](const char *) { Passthrough.printStatus(Serial0); }},
    {"BRIDGE_MOUSE_MODE", handleBridgeMouseMode},
    {"km.version", handleKmVersionCommand},
    {"km.echo", handleKmEchoCommand},
    {"km.buttons", handleKmButtonsCommand},
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
    {"USB_PROFILE_NOT_READY", [](const char *) {
        vTaskDelay(pdMS_TO_TICKS(50));
        sendBridgeText("sendPassthroughProfile");
    }},
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

void trimCommand(char* command) {
    int len = strlen(command);
    while (len > 0 && (command[len - 1] == ' ' || command[len - 1] == '\n' || command[len - 1] == '\r')) {
        command[len - 1] = '\0';
        len--;
    }
}

void serial0RX() {
    uint8_t traceChunk[64];
    size_t traceLength = 0;
    const auto flushTrace = [&]() {
        if (traceLength != 0) {
            MakcuWireTrace::record(MakcuWireTrace::Direction::Rx,
                                   traceChunk, traceLength);
            traceLength = 0;
        }
    };

    while (Serial0.available() > 0) {
        const uint8_t rawByte = static_cast<uint8_t>(Serial0.read());
        // Secondary begins every session with this fixed 9-byte baud switch.
        // Start before saving 0xDE so no handshake byte is lost.
        if (!MakcuWireTrace::active() && rawByte == 0xDE) {
            MakcuWireTrace::start(true);
        }
        if (MakcuWireTrace::active()) {
            traceChunk[traceLength++] = rawByte;
            if (traceLength == sizeof(traceChunk)) {
                flushTrace();
            }
        }
        if (consumeMakcuSpeedSwitchByte(rawByte)) {
            continue;
        }
        char byte = static_cast<char>(rawByte);

        if (byte == '\r') {
            continue;
        }

        if (!serial0RingBuffer.isFull()) {
            serial0RingBuffer.push(byte);
        } else {
            Serial0.println("Serial0 ring buffer overflow detected.");
        }

        if (byte == '\n') {
            // Preserve RX-before-TX ordering when the command handler replies.
            flushTrace();
            char commandBuffer[620];
            int commandIndex = 0;

            while (!serial0RingBuffer.isEmpty() && commandIndex < sizeof(commandBuffer) - 1) {
                serial0RingBuffer.pop(commandBuffer[commandIndex++]);
            }

            commandBuffer[commandIndex] = '\0';

            trimCommand(commandBuffer);

            // CH343 can leave one or two stale bytes in the UART FIFO across
            // a host reopen/baud transition. Resynchronize on the ASCII Makcu
            // command prefix instead of letting that residue turn a valid
            // command into an unknown debug echo.
            char *kmPrefix = strstr(commandBuffer, "km.");
            if (kmPrefix != nullptr && kmPrefix != commandBuffer) {
                memmove(commandBuffer, kmPrefix, strlen(kmPrefix) + 1);
            }

            if (strncmp(commandBuffer, "km.move", 7) == 0) {
                handleKmMoveCommand(commandBuffer);
            } else {
                processCommand(commandBuffer);
            }
        }
    }
    flushTrace();
}

void serial1RX() {
    while (Serial1.available() > 0) {
        const uint8_t rawByte = static_cast<uint8_t>(Serial1.read());
        if (consumeBridgeSerialByte(rawByte)) {
            continue;
        }
        char byte = static_cast<char>(rawByte);

        if (byte == '\r') {
            continue;
        }

        if (!serial1RingBuffer.isFull()) {
            serial1RingBuffer.push(byte);
        } else {
            Serial0.println("Serial1 ring buffer overflow detected.");
        }

        if (byte == '\n') {
            char commandBuffer[620];
            int commandIndex = 0;

            while (!serial1RingBuffer.isEmpty() && commandIndex < sizeof(commandBuffer) - 1) {
                serial1RingBuffer.pop(commandBuffer[commandIndex++]);
            }

            commandBuffer[commandIndex] = '\0';

            trimCommand(commandBuffer);

            observeRightMouseCommand(commandBuffer);

            if (strncmp(commandBuffer, "km.move", 7) == 0) {
                handleKmMoveCommand(commandBuffer);
            } else {
                processCommand(commandBuffer);
            }
        }
    }
}


void processRingBufferCommand(RingBuf<char, 620> &buffer) {
    char commandBuffer[620];
    int commandIndex = 0;
    char byte;

    while (!buffer.isEmpty()) {
        buffer.pop(byte);
        
        Serial0.print("Popped Byte: '");
        Serial0.print(byte);
        Serial0.print("' (ASCII: ");
        Serial0.print((int)byte); 
       
        if (byte == '\n' || commandIndex >= 620 - 1) {
            break;
        }

        commandBuffer[commandIndex++] = byte;
    }

    commandBuffer[commandIndex] = '\0'; 
    trimCommand(commandBuffer);
    
    Serial0.print("Full Command: ");
    Serial0.println(commandBuffer);

    if (commandIndex > 0) {
        processCommand(commandBuffer);
    }
}


void handleKmMoveCommand(const char *command) {
    int x = 0;
    int y = 0;
    if (command != nullptr &&
        sscanf(command + strlen("km.move") + 1, "%d,%d", &x, &y) == 2) {
        // Passthrough.mouseMove() is now a constant-time relative accumulator.
        // Processing here preserves every command even when multiple UART
        // lines arrive before a lower-priority task gets scheduled.
        handleMove(x, y);
    }
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
    if (processingUsbCommands) {
        return;
    }
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
    sendBridgeText(command);
    currentCommandIndex++;
    if (currentCommandIndex >= sizeof(commandQueue) / sizeof(commandQueue[0])) {
        usbReady = false;
        processingUsbCommands = false;
        InitUSB();
        vTaskDelay(700);
        serial0Locked = false;
        sendBridgeText("USB_INIT");
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

    // DSE uses a fixed USB identity and enumerates independently of the legacy
    // descriptor exchange. Control-port commands must remain live even if an
    // older right-side descriptor sequence is still in progress.
    for (const auto &entry : normalCommandTable) {
        if (strncmp(command, entry.command, strlen(entry.command)) == 0) {
            entry.handler(command);
            return;
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
    if (strcmp(command, "DEBUG_STATUS") == 0) {
        sendBridgeText(command);
    } else if (strcmp(command, "DEBUG_ON") == 0) {
        sendBridgeText("DEBUG_ON");
    } else if (strcmp(command, "DEBUG_OFF") == 0) {
        sendBridgeText("DEBUG_OFF");
    } else if (sscanf(command + strlen("DEBUG_"), "%d", &debugLevel) == 1) {
        sendBridgeText(command);
    } else {
        Serial0.println("Invalid DEBUG command. Bytes received:");
        for (int i = 0; i < strlen(command); i++) {
            Serial0.print("Byte ");
            Serial0.print(i);
            Serial0.print(": '");
            Serial0.print(command[i]); 
            Serial0.print("' (ASCII: ");
            Serial0.print((int)command[i]);
            Serial0.println(")");
        }
    }
}


void handleDebugcommand(const char *command) {
    // km.echo(0) promises an echo-free production link. Re-emitting malformed
    // binary/text fragments can create an output storm and further starve the
    // control UART while the controller bridge is active.
    (void)command;
}

void handleNoDevice(const char *command)
{
    deviceConnected = false;
}

void handleKmMoveto(const char *command) {
    int x = 0;
    int y = 0;
    if (command != nullptr &&
        sscanf(command + strlen("km.moveto") + 1, "%d,%d", &x, &y) == 2) {
        handleMoveto(x, y);
    }
}

void handleKmGetpos(const char *command) {
    handleGetPos();
}

void handleKmMouseButtonLeft1(const char *command) {
    handleMouseButton(MOUSE_BUTTON_LEFT, true);
}

void handleBridgeMouseMode(const char *command) {
    const char *mode = command + strlen("BRIDGE_MOUSE_MODE");
    while (*mode == ' ' || *mode == '=') ++mode;

    bool enabled = true;
    uint16_t rateHz = 0;
    if (strcmp(mode, "off") == 0) {
        enabled = false;
    } else if (strcmp(mode, "unlimited") == 0 ||
               strcmp(mode, "on") == 0) {
        rateHz = 0;
    } else if (strcmp(mode, "125") == 0) {
        rateHz = 125;
    } else if (strcmp(mode, "250") == 0) {
        rateHz = 250;
    } else if (strcmp(mode, "500") == 0) {
        rateHz = 500;
    } else {
        Serial0.println(
            "BRIDGE_MOUSE_MODE error expected=off|unlimited|125|250|500");
        return;
    }

    Passthrough.configureMouseInjection(enabled, rateHz);
    if (!enabled) {
        Serial0.println("BRIDGE_MOUSE_MODE ok mode=off rate=0");
    } else if (rateHz == 0) {
        Serial0.println("BRIDGE_MOUSE_MODE ok mode=unlimited rate=0");
    } else {
        Serial0.printf("BRIDGE_MOUSE_MODE ok mode=limited rate=%u\r\n",
                       static_cast<unsigned>(rateHz));
    }
}

void handleKmMouseButtonLeft0(const char *command) {
    handleMouseButton(MOUSE_BUTTON_LEFT, false);
}

void handleKmMouseButtonRight1(const char *command) {
    handleMouseButton(MOUSE_BUTTON_RIGHT, true);
}

void handleKmMouseButtonRight0(const char *command) {
    handleMouseButton(MOUSE_BUTTON_RIGHT, false);
}

void handleKmMouseButtonMiddle1(const char *command) {
    handleMouseButton(MOUSE_BUTTON_MIDDLE, true);
}

void handleKmMouseButtonMiddle0(const char *command) {
    handleMouseButton(MOUSE_BUTTON_MIDDLE, false);
}

void handleKmMouseButtonForward1(const char *command) {
    handleMouseButton(MOUSE_BUTTON_FORWARD, true);
}

void handleKmMouseButtonForward0(const char *command) {
    handleMouseButton(MOUSE_BUTTON_FORWARD, false);
}

void handleKmMouseButtonBackward1(const char *command) {
    handleMouseButton(MOUSE_BUTTON_BACKWARD, true);
}

void handleKmMouseButtonBackward0(const char *command) {
    handleMouseButton(MOUSE_BUTTON_BACKWARD, false);
}

void handleKmWheel(const char *command) {
    int wheelMovement = 0;
    if (command != nullptr &&
        sscanf(command + strlen("km.wheel") + 1, "%d", &wheelMovement) == 1) {
        handleMouseWheel(wheelMovement);
    }
}

void handleMove(int x, int y) {
    if (!lockMouseCommands()) return;
    int64_t nextX = 0;
    int64_t nextY = 0;
    const bool representable =
        !__builtin_add_overflow(mouseX, static_cast<int64_t>(x), &nextX) &&
        !__builtin_add_overflow(mouseY, static_cast<int64_t>(y), &nextY);
    if (representable && Passthrough.mouseMove(x, y)) {
        mouseX = nextX;
        mouseY = nextY;
    }
    unlockMouseCommands();
}

void handleMoveto(int x, int y) {
    if (!lockMouseCommands()) return;
    int64_t deltaX = 0;
    int64_t deltaY = 0;
    const int64_t targetX = x;
    const int64_t targetY = y;
    const bool representable =
        !__builtin_sub_overflow(targetX, mouseX, &deltaX) &&
        !__builtin_sub_overflow(targetY, mouseY, &deltaY);
    if (representable && Passthrough.mouseMove(deltaX, deltaY)) {
        mouseX = targetX;
        mouseY = targetY;
    }
    unlockMouseCommands();
}

void handleMouseButton(uint8_t button, bool press) {
    Passthrough.mouseButton(button, press);
}

void handleMouseWheel(int wheelMovement) {
    Passthrough.mouseMove(0, 0, wheelMovement);
}

void handleGetPos() {
    if (!lockMouseCommands()) return;
    Serial0.printf("km.pos(%lld,%lld)\r\n",
                   static_cast<long long>(mouseX),
                   static_cast<long long>(mouseY));
    unlockMouseCommands();
}
