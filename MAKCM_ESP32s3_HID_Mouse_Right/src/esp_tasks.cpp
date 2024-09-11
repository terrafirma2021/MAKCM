#include "EspUsbHost.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

const int MaxLogQueSize = 512;
QueueHandle_t logQueue;
#define USB_TASK_PRIORITY 1
#define CLIENT_TASK_PRIORITY 2
const unsigned long ledFlashTime = 25; // Set The LED Flash timer in ms
SemaphoreHandle_t ledSemaphore;

void EspUsbHost::begin(void)
{
    usbTransferSize = 0;
    deviceSuspended = false;
    last_activity_time = millis();

    const usb_host_config_t host_config = {
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };

    usb_host_install(&host_config);

    logQueue = xQueueCreate(LOG_QUEUE_SIZE, MaxLogQueSize);
    txQueue = xQueueCreate(LOG_QUEUE_SIZE, LOG_MESSAGE_SIZE);

    ledSemaphore = xSemaphoreCreateBinary();

    xTaskCreate(processLogQueue, "Log Processing", 4096, this, 4, &logTaskHandle);

    xTaskCreate([](void *arg) {
        static_cast<EspUsbHost *>(arg)->tx_esp(arg);
    }, "tx_esp", 3400, this, 4, &espTxTaskHandle);

    xTaskCreate([](void *arg) {
        static_cast<EspUsbHost *>(arg)->rx_esp_serial0(arg);
    }, "RxTaskSerial0", 3400, this, 5, NULL);

    xTaskCreate(rx_esp, "RxTask", 2900, this, 5, NULL);
    xTaskCreate(usb_lib_task, "usbLibTask", 4096, this, USB_TASK_PRIORITY, NULL);
    xTaskCreate(usb_client_task, "usbClientTask", 4096, this, CLIENT_TASK_PRIORITY, NULL);
    xTaskCreate(monitor_inactivity_task, "MonitorInactivityTask", 3200, this, 3, NULL);
    xTaskCreate(ledFlashTask, "LED Flash Task", 1500, NULL, 1, NULL);
}

void EspUsbHost::rx_esp_serial0(void *command)
{
    EspUsbHost *instance = static_cast<EspUsbHost *>(command);
    while (true)
    {
        if (Serial0.available())
        {
            String incomingCommand = Serial0.readStringUntil('\n');
            incomingCommand.trim();
            if (incomingCommand.length() > 0)
            {
                instance->handleIncomingCommands(incomingCommand);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

void EspUsbHost::tx_esp(void *arg)
{
    EspUsbHost *instance = static_cast<EspUsbHost *>(arg);
    char logMsg[LOG_MESSAGE_SIZE];
    uint32_t notificationValue;

    while (true)
    {
        xTaskNotifyWait(0, ULONG_MAX, &notificationValue, portMAX_DELAY);

        while (xQueueReceive(instance->txQueue, &logMsg, portMAX_DELAY) == pdPASS)
        {
            Serial1.print(logMsg);
        }
    }
}

void EspUsbHost::rx_esp(void *command)
{
    EspUsbHost *instance = static_cast<EspUsbHost *>(command);
    while (true)
    {
        if (Serial1.available())
        {
            String incomingCommand = Serial1.readStringUntil('\n');
            incomingCommand.trim();
            if (incomingCommand.length() > 0)
            {
                instance->handleIncomingCommands(incomingCommand);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

bool EspUsbHost::tx_Que(const char *format, ...)
{
    char logMsg[LOG_MESSAGE_SIZE];
    va_list args;
    va_start(args, format);
    int logLength = vsnprintf(logMsg, sizeof(logMsg), format, args);
    va_end(args);

    if (logLength >= sizeof(logMsg))
    {
        logLength = sizeof(logMsg) - 1;
    }

    logMsg[logLength] = '\0';

    if (xQueueSend(txQueue, logMsg, 0) == pdPASS)
    {
        xTaskNotifyGive(espTxTaskHandle);
        return true;
    }
    else
    {
        return false;
    }
}

void EspUsbHost::monitor_inactivity_task(void *arg)
{
    EspUsbHost *usbHost = static_cast<EspUsbHost *>(arg);

    while (true)
    {
        vTaskDelay(pdMS_TO_TICKS(100));

        if (EspUsbHost::deviceConnected && !usbHost->deviceSuspended && millis() - usbHost->last_activity_time > 10000)
        {
            usbHost->suspend_device();
        }
    }
}

void ledFlashTask(void *parameter)
{
    pinMode(9, OUTPUT);

    while (true)
    {
        if (xSemaphoreTake(ledSemaphore, portMAX_DELAY) == pdTRUE)
        {
            digitalWrite(9, HIGH);
            vTaskDelay(ledFlashTime / portTICK_PERIOD_MS);
            digitalWrite(9, LOW);
            vTaskDelay(ledFlashTime / portTICK_PERIOD_MS);
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
