#include "EspUsbHost.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"


// Buffers defined here
RingBuf<char, 512> rxBuffer;

SemaphoreHandle_t ledSemaphore;
//extern serialMutex = xSemaphoreCreateMutex();

TaskHandle_t rxSerialTaskHandle;
#define USB_TASK_PRIORITY 1
#define CLIENT_TASK_PRIORITY 2

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
    serialMutex = xSemaphoreCreateMutex();

    xTaskCreate([](void *arg) {
        static_cast<EspUsbHost *>(arg)->receiveSerial0(arg);
    }, "RxTaskSerial0", 4096, this, 5, &rxSerialTaskHandle);

    xTaskCreate([](void *arg) {
        static_cast<EspUsbHost *>(arg)->usbLibraryTask(arg);
        Serial0.println("usbLibTask task completed.");
    }, "usbLibTask", 4096, this, USB_TASK_PRIORITY, NULL);

    xTaskCreate([](void *arg) {
        static_cast<EspUsbHost *>(arg)->usbClientTask(arg);
        Serial0.println("usbClientTask task completed.");
    }, "usbClientTask", 4096, this, CLIENT_TASK_PRIORITY, NULL);

    xTaskCreate(receiveSerial1, "RxTaskSerial1", 4096, this, 5, NULL);
    xTaskCreate(monitorInactivity, "MonitorInactivityTask", 3200, this, 3, NULL);
    xTaskCreate(flashLEDToggleTask, "LED Flash Task", 1500, NULL, 1, NULL);

    Serial0.println("EspUsbHost::begin() completed. Tasks created.");
}

void EspUsbHost::receiveSerial0(void *command)
{
    EspUsbHost *instance = static_cast<EspUsbHost *>(command);

    while (true)
    {
        vTaskDelay(pdMS_TO_TICKS(1));

        if (Serial0.available() > 0)
        {
            while (Serial0.available() > 0)
            {
                char byte = Serial0.read();

                if (byte == '\r') continue;

                if (!rxBuffer.isFull()) 
                {
                    rxBuffer.push(byte);
                }
                else 
                {
                    Serial0.println("RX buffer overflow detected.");
                }

                if (byte == '\n')  
                {
                    char commandBuffer[620];
                    int commandIndex = 0;

                    while (!rxBuffer.isEmpty() && commandIndex < sizeof(commandBuffer) - 1)
                    {
                        rxBuffer.pop(commandBuffer[commandIndex++]);

                        if (commandBuffer[commandIndex - 1] == '\n') break;
                    }

                    commandBuffer[commandIndex] = '\0';

                    if (commandIndex > 0 && commandBuffer[commandIndex - 1] == '\n') 
                    {
                        commandBuffer[commandIndex - 1] = '\0';
                    }
                    instance->handleIncomingCommands(commandBuffer);
                }
            }
        }
    }
}

void EspUsbHost::receiveSerial1(void *command)
{
    while (true)
    {
        while (Serial1.available() > 0)
        {
            char byte = Serial1.read();
            if (byte == '\r') continue;

            if (!rxBuffer.isFull()) 
            {
                rxBuffer.push(byte);
            }
            else 
            {
                Serial0.println("RX buffer overflow detected.");
            }

            if (byte == '\n')
            {
                char commandBuffer[620];
                int commandIndex = 0;

                while (!rxBuffer.isEmpty() && commandIndex < sizeof(commandBuffer) - 1)
                {
                    rxBuffer.pop(commandBuffer[commandIndex++]);
                }
                
                commandBuffer[commandIndex] = '\0';

                if (commandIndex > 0 && commandBuffer[commandIndex - 1] == '\n') 
                {
                    commandBuffer[commandIndex - 1] = '\0';
                }

                static_cast<EspUsbHost *>(command)->handleIncomingCommands(commandBuffer);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

void EspUsbHost::monitorInactivity(void *arg)
{
    EspUsbHost *usbHost = static_cast<EspUsbHost *>(arg);

    while (true)
    {
        vTaskDelay(pdMS_TO_TICKS(100));

        if (usbHost->deviceConnected && !usbHost->deviceSuspended && millis() - usbHost->last_activity_time > 10000)
        {
            usbHost->suspend_device();
        }
    }
}

void EspUsbHost::usbLibraryTask(void *arg)
{
    EspUsbHost *instance = static_cast<EspUsbHost *>(arg);

    while (true)
    {
        uint32_t event_flags;
        esp_err_t err = usb_host_lib_handle_events(portMAX_DELAY, &event_flags);

        if (err != ESP_OK)
        {
            instance->log(LOG_LEVEL_ERROR, "usb_host_lib_handle_events() err=%x", err);
            continue;
        }

        if (instance->clientHandle == NULL || !instance->isClientRegistering)
        {
            instance->log(LOG_LEVEL_INFO, "Inform: Registering client...");
            const usb_host_client_config_t client_config = {
                .max_num_event_msg = 10,
                .async = {
                    .client_event_callback = instance->_clientEventCallback,
                    .callback_arg = instance,
                }};

            err = usb_host_client_register(&client_config, &instance->clientHandle);
            instance->log(LOG_LEVEL_INFO, "Inform: usb_host_client_register() status: %d", err);
            if (err != ESP_OK)
            {
                instance->log(LOG_LEVEL_WARN, "Warn: Failed to re-register client, retrying...");
                vTaskDelay(100);
            }
            else
            {
                instance->log(LOG_LEVEL_INFO, "Inform: Client registered successfully.");
                instance->isClientRegistering = true;
            }
        }
    }
}

void EspUsbHost::usbClientTask(void *arg)
{
    EspUsbHost *instance = static_cast<EspUsbHost *>(arg);

    while (true)
    {
        if (!instance->isClientRegistering)
        {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        esp_err_t err = usb_host_client_handle_events(instance->clientHandle, portMAX_DELAY);
    }
}

void flashLEDToggleTask(void *parameter)
{
    pinMode(9, OUTPUT);

    while (true)
    {
        if (xSemaphoreTake(ledSemaphore, portMAX_DELAY) == pdTRUE)
        {
            digitalWrite(9, HIGH);
            vTaskDelay(25 / portTICK_PERIOD_MS);
            digitalWrite(9, LOW);
        }
    }
}

void flashLED()
{
    if (ledSemaphore != NULL)
    {
        xSemaphoreGive(ledSemaphore);
    }
}