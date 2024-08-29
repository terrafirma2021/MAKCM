#include "main.h"

extern void InitUSB();
extern void requestUSBDescriptors();
extern void processCombinedBuffer();

TaskHandle_t serial1TaskHandle;
TaskHandle_t serial0TaskHandle;
TaskHandle_t processCombinedBufferTaskHandle;


void setup() {
    delay(1100); 
    Serial0.begin(115200);
    pinMode(9, OUTPUT);
    digitalWrite(9, LOW);

    Serial1.begin(5000000, SERIAL_8N1, 1, 2);
    Serial1.println("READY");
    Serial0.println("Left MCU Started");

    // Create tasks for handling serial communication
    BaseType_t xReturned;

    xReturned = xTaskCreate(serial1Task, "Serial1Task", 2048, NULL, 1, &serial1TaskHandle);
    if (xReturned != pdPASS) {
        Serial0.println("Failed to create Serial1Task");
    }

    xReturned = xTaskCreate(serial0Task, "Serial0Task", 2048, NULL, 2, &serial0TaskHandle);
    if (xReturned != pdPASS) {
        Serial0.println("Failed to create Serial0Task");
    }

    xReturned = xTaskCreate(processCombinedBufferTask, "ProcessCombinedBufferTask", 3096, NULL, 3, &processCombinedBufferTaskHandle);
    if (xReturned != pdPASS) {
        Serial0.println("Failed to create ProcessCombinedBufferTask");
    }
}


// All whiles are non blocking as they are assigned tasks :) // Free the RTOS #

// Task for handling Serial0 communication
void serial0Task(void *pvParameters) {
    while (true) {
        serial0RX();
        vTaskDelay(1 / portTICK_PERIOD_MS);
    }
}

// Task for handling Serial1 communication
void serial1Task(void *pvParameters) {
    while (true) {
        serial1RX();
        vTaskDelay(1 / portTICK_PERIOD_MS);
    }
}

// Task for processing the combined buffer
void processCombinedBufferTask(void *pvParameters) {
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        processCombinedBuffer();
    }
}

void loop() {
    requestUSBDescriptors();
}
