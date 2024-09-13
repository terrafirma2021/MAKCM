#include "EspUsbHost.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

// Buffers defined here
RingBuf<char, 512> rxBuffer;
RingBuf<char, 512> txBuffer;
SemaphoreHandle_t ledSemaphore;
TaskHandle_t cleanupTaskHandle = NULL;
TaskHandle_t rxSerialTaskHandle;
#define USB_TASK_PRIORITY 1
#define CLIENT_TASK_PRIORITY 2

// Utility function to handle reading from a serial port
void handleSerialInput(HardwareSerial &serial, EspUsbHost *instance) {
    while (serial.available() > 0) {
        char byte = serial.read();

        if (byte == '\r') continue;

        if (!rxBuffer.isFull()) {
            rxBuffer.push(byte);
        } else {
            ESP_LOGW("EspUsbHost", "RX buffer overflow detected.");
            break; // Stop pushing if buffer is full
        }

        if (byte == '\n') {
            char commandBuffer[620];
            int commandIndex = 0;

            while (!rxBuffer.isEmpty() && commandIndex < sizeof(commandBuffer) - 1) {
                rxBuffer.pop(commandBuffer[commandIndex++]);
                if (commandBuffer[commandIndex - 1] == '\n') break;
            }

            commandBuffer[commandIndex] = '\0';

            if (commandIndex > 0 && commandBuffer[commandIndex - 1] == '\n') {
                commandBuffer[commandIndex - 1] = '\0'; // Remove trailing newline
            }

            instance->handleIncomingCommands(commandBuffer);
        }
    }
}

void EspUsbHost::begin(void)
{
    usbTransferSize = 0;
    deviceSuspended = false;
    last_activity_time = millis();

    const usb_host_config_t host_config = {
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };

    usb_host_install(&host_config);

    ledSemaphore = xSemaphoreCreateBinary();

    // Task creation with error handling
    if (xTaskCreate([](void *arg) { 
        static_cast<EspUsbHost *>(arg)->receiveSerial0(arg); 
    }, "RxTaskSerial0", 4096, this, 5, &rxSerialTaskHandle) != pdPASS) {
        ESP_LOGE("EspUsbHost", "Failed to create RxTaskSerial0.");
    }

    if (xTaskCreate([](void *arg) { 
        static_cast<EspUsbHost *>(arg)->receiveSerial1(arg); 
    }, "RxTaskSerial1", 4096, this, 5, NULL) != pdPASS) {
        ESP_LOGE("EspUsbHost", "Failed to create RxTaskSerial1.");
    }

    if (xTaskCreate([](void *arg) { 
        static_cast<EspUsbHost *>(arg)->usbLibraryTask(arg); 
    }, "usbLibTask", 4096, this, USB_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE("EspUsbHost", "Failed to create usbLibTask.");
    }

    if (xTaskCreate([](void *arg) { 
        static_cast<EspUsbHost *>(arg)->usbClientTask(arg); 
    }, "usbClientTask", 4096, this, CLIENT_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE("EspUsbHost", "Failed to create usbClientTask.");
    }

    if (xTaskCreate([](void *arg) { 
        static_cast<EspUsbHost *>(arg)->cleanupTask(arg); 
    }, "CleanupTask", 4096, this, 5, &cleanupTaskHandle) != pdPASS) {
        ESP_LOGE("EspUsbHost", "Failed to create CleanupTask.");
    }

    if (xTaskCreate([](void *arg) { 
        static_cast<EspUsbHost *>(arg)->monitorInactivity(arg); 
    }, "MonitorInactivityTask", 3200, this, 3, NULL) != pdPASS) {
        ESP_LOGE("EspUsbHost", "Failed to create MonitorInactivityTask.");
    }

    if (xTaskCreate(flashLEDToggleTask, "LED Flash Task", 1500, NULL, 1, NULL) != pdPASS) {
        ESP_LOGE("EspUsbHost", "Failed to create LED Flash Task.");
    }

    ESP_LOGI("EspUsbHost", "EspUsbHost::begin() completed. Tasks created.");
}

void EspUsbHost::receiveSerial0(void *arg)
{
    EspUsbHost *instance = static_cast<EspUsbHost *>(arg);
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10));  // Prevent task from hogging CPU
        handleSerialInput(Serial0, instance);  // Use the shared serial handling function
    }
}

void EspUsbHost::receiveSerial1(void *arg)
{
    EspUsbHost *instance = static_cast<EspUsbHost *>(arg);
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10));  // Prevent task from hogging CPU
        handleSerialInput(Serial1, instance);  // Use the shared serial handling function
    }
}

bool EspUsbHost::serial1Send(const char *format, ...)
{
    char logMsg[620]; // Buffer to store the formatted message

    // Format the message using variadic arguments
    va_list args;
    va_start(args, format);
    vsnprintf(logMsg, sizeof(logMsg), format, args);
    va_end(args);

    // Push the message into the TX buffer and send it via Serial1
    for (int i = 0; i < strlen(logMsg); ++i) {
        if (!txBuffer.isFull()) {
            txBuffer.push(logMsg[i]);
        } else {
            ESP_LOGW("EspUsbHost", "TX buffer overflow detected.");
            return false; // Stop sending if the buffer is full
        }
    }

    // Send the contents of the buffer through Serial1
    while (!txBuffer.isEmpty()) {
        char byte;
        txBuffer.pop(byte);
        Serial1.write(byte);
    }

    return true; // Successfully sent the message
}

void EspUsbHost::monitorInactivity(void *arg)
{
    EspUsbHost *usbHost = static_cast<EspUsbHost *>(arg);

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(100));

        if (usbHost->deviceConnected && !usbHost->deviceSuspended && millis() - usbHost->last_activity_time > 10000) {
            usbHost->suspend_device();
        }
    }
}

void EspUsbHost::usbLibraryTask(void *arg)
{
    EspUsbHost *instance = static_cast<EspUsbHost *>(arg);

    while (true) {
        uint32_t event_flags;
        esp_err_t err = usb_host_lib_handle_events(portMAX_DELAY, &event_flags);

        if (err != ESP_OK) {
            ESP_LOGE("EspUsbHost", "usb_host_lib_handle_events() failed with error: %x", err);
            continue;
        }

        if (instance->clientHandle == NULL || !instance->isClientRegistering) {
            ESP_LOGI("EspUsbHost", "Registering client...");
            const usb_host_client_config_t client_config = {
                .max_num_event_msg = 10,
                .async = {
                    .client_event_callback = instance->_clientEventCallback,
                    .callback_arg = instance,
                }
            };

            err = usb_host_client_register(&client_config, &instance->clientHandle);
            if (err != ESP_OK) {
                ESP_LOGW("EspUsbHost", "Failed to register client, retrying...");
                vTaskDelay(pdMS_TO_TICKS(100)); // Add delay before retrying
            } else {
                ESP_LOGI("EspUsbHost", "Client registered successfully.");
                instance->isClientRegistering = true;
            }
        }
    }
}


void EspUsbHost::usbClientTask(void *arg)
{
    EspUsbHost *instance = static_cast<EspUsbHost *>(arg);

    while (true) {
        if (!instance->isClientRegistering) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        usb_host_client_handle_events(instance->clientHandle, portMAX_DELAY);
    }
}

void flashLEDToggleTask(void *parameter)
{
    pinMode(9, OUTPUT);

    while (true) {
        if (xSemaphoreTake(ledSemaphore, portMAX_DELAY) == pdTRUE) {
            digitalWrite(9, HIGH);
            vTaskDelay(pdMS_TO_TICKS(25));  // Use FreeRTOS delay in milliseconds
            digitalWrite(9, LOW);
        }
    }
}

void flashLED()
{
    if (ledSemaphore != NULL) {
        xSemaphoreGive(ledSemaphore);
    }
}
