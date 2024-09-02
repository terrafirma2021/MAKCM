#ifndef MAIN_H
#define MAIN_H

#include <Arduino.h>
#include <USB.h>
#include <USBHIDMouse.h>
#include "handleCommands.h"
#include "InitSettings.h"
#include "USBSetup.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Declare task handles using extern to avoid redefinition errors
extern TaskHandle_t serial1TaskHandle;
extern TaskHandle_t serial0TaskHandle;
extern TaskHandle_t mouseMoveTaskHandle;
extern TaskHandle_t ledFlashTaskHandle;

// Function prototypes for tasks and ISRs
void serial0Task(void *pvParameters);
void serial1Task(void *pvParameters);
void mouseMoveTask(void *pvParameters);
void ledFlashTask(void *pvParameters);

// ISR declarations
void IRAM_ATTR serial0ISR();
void IRAM_ATTR serial1ISR();

// Other function prototypes
void tasks();
void serial0RX();
void serial1RX();
void requestUSBDescriptors();

#endif