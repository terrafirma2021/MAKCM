#include "main.h"

#define NOTIFY_SERIAL0 1
#define NOTIFY_SERIAL1 2

TaskHandle_t serial1TaskHandle = NULL;
TaskHandle_t serial0TaskHandle = NULL;
TaskHandle_t ledFlashTaskHandle = NULL;



void tasks() {
    BaseType_t xReturned;
    initializeCommandState();

xReturned = xTaskCreate(serial1Task, "Serial1Task", 4096, NULL, 6, &serial1TaskHandle);
if (xReturned != pdPASS) {
    Serial0.println("Failed to create Serial1Task");
}

    // Descriptor/status commands use printf and several nested parser calls.
    // 2160 bytes overflowed during BRIDGE_STATUS and reset the proxy.
    // Stay below the controller bridge task (priority 6), but drain UART0
    // often enough that high-rate Secondary bursts cannot fill its RX buffer.
    xReturned = xTaskCreate(serial0Task, "Serial0Task", 4096, NULL, 5, &serial0TaskHandle);
    if (xReturned != pdPASS) {
        Serial0.println("Failed to create Serial0Task");
    }

    xReturned = xTaskCreate(ledFlashTask, "LEDFlashTask", 1536, NULL, 1, &ledFlashTaskHandle);
    if (xReturned != pdPASS) {
        Serial0.println("Failed to create LEDFlashTask");
    }
}

void IRAM_ATTR serial0ISR() {
    if (serial0TaskHandle != nullptr) {
        xTaskNotifyGive(serial0TaskHandle);
    }
}

void IRAM_ATTR serial1ISR() {
    // HardwareSerial::onReceive executes in Arduino's UART event task, not in
    // interrupt context. Use the task-context API accordingly.
    if (serial1TaskHandle != nullptr) {
        xTaskNotifyGive(serial1TaskHandle);
    }
}

void serial0Task(void *pvParameters) {
    while (true) {
        serial0RX();
        // Polling avoids Arduino's priority-24 UART event task for UART0. One
        // millisecond is far below the 4096-byte buffer's capacity at 4 Mbps.
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

void serial1Task(void *pvParameters) {
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        serial1RX();
    }
}

