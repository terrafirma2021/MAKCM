#include "main.h"

#define NOTIFY_SERIAL0 1
#define NOTIFY_SERIAL1 2

TaskHandle_t serial1TaskHandle = NULL;
TaskHandle_t serial0TaskHandle = NULL;
TaskHandle_t mouseMoveTaskHandle = NULL;
TaskHandle_t ledFlashTaskHandle = NULL;



void tasks() {
    BaseType_t xReturned;

xReturned = xTaskCreate(serial1Task, "Serial1Task", 2500, NULL, 1, &serial1TaskHandle);
if (xReturned != pdPASS) {
    Serial0.println("Failed to create Serial1Task");
}

    xReturned = xTaskCreate(serial0Task, "Serial0Task", 2160, NULL, 2, &serial0TaskHandle);
    if (xReturned != pdPASS) {
        Serial0.println("Failed to create Serial0Task");
    }

    xReturned = xTaskCreate(mouseMoveTask, "MouseMoveTask", 1536, NULL, 3, &mouseMoveTaskHandle);
    if (xReturned != pdPASS) {
        Serial0.println("Failed to create MouseMoveTask");
    }

    xReturned = xTaskCreate(ledFlashTask, "LEDFlashTask", 1536, NULL, 1, &ledFlashTaskHandle);
    if (xReturned != pdPASS) {
        Serial0.println("Failed to create LEDFlashTask");
    }
}

void IRAM_ATTR serial0ISR() {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    vTaskNotifyGiveFromISR(serial0TaskHandle, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

void IRAM_ATTR serial1ISR() {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    vTaskNotifyGiveFromISR(serial1TaskHandle, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

void serial0Task(void *pvParameters) {
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        serial0RX();
    }
}

void serial1Task(void *pvParameters) {
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        serial1RX();
    }
}

