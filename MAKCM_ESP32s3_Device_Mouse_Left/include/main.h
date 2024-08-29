#ifndef MAIN_H
#define MAIN_H

#include <Arduino.h>
#include <USB.h>
#include <USBHIDMouse.h>
#include "MouseMove.h"
#include "InitSettings.h"
#include "USBSetup.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Task handles
extern TaskHandle_t serial1TaskHandle;
extern TaskHandle_t serial0TaskHandle;
extern TaskHandle_t processCombinedBufferTaskHandle;

// Function declarations
void setup();
void loop();
void serial0Task(void *pvParameters);
void serial1Task(void *pvParameters);
void processCombinedBufferTask(void *pvParameters);

#endif // MAIN_H
