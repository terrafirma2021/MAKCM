#include "EspUsbHost.h"
#include "esp_task_wdt.h"
#include <driver/timer.h>
#include "freertos/queue.h"
#include "esp_log.h"
#include <stdarg.h>
#include <stdio.h>
#include <sstream>
#include <iomanip>
#include "freertos/semphr.h"

bool enableYield = false;

void EspUsbHost::handleIncomingCommands(const String &command)
{
    if (command == "DEBUG_ON")
    {
        debugModeActive = true;
        EspUsbHost::current_log_level = LOG_LEVEL_FIXED;
        tx_Que("Debug mode activated.\n");

        // log(LOG_LEVEL_DEBUG, "Debug mode active. Please remove USB mouse.");  TO DO!!
    }
    else if (command == "DEBUG_OFF")
    {
        debugModeActive = false;

        log(LOG_LEVEL_DEBUG, "ESPLOG Inform: Debug mode off. Resetting system.");

        Serial1.print("USB_GOODBYE\n");

        vTaskDelay(pdMS_TO_TICKS(100));

        esp_restart();
    }
    else if (command.startsWith("DEBUG_"))
    {
        int level = command.substring(6).toInt();
        if (level >= LOG_LEVEL_OFF && level <= LOG_LEVEL_PARSED)
        {
            EspUsbHost::current_log_level = level;
            tx_Que("Log level set to %d\n", EspUsbHost::current_log_level);
        }
        else
        {
            tx_Que("Invalid log level: %d\n", level);
        }
    }
    else if (command == "READY")
    {
        if (EspUsbHost::deviceConnected)
        {
            tx_Que("USB_HELLO\n");
        }
        else
        {
            tx_Que("USB_ISNULL\n");
        }
    }
    else if (command == "USB_INIT")
    {
        deviceMouseReady = true;
    }
    else if (command == "sendDeviceInfo")
    {
        sendDeviceInfo();
    }
    else if (command == "sendDescriptorDevice")
    {
        sendDescriptorDevice();
    }
    else if (command == "sendEndpointDescriptors")
    {
        sendEndpointDescriptors();
    }
    else if (command == "sendInterfaceDescriptors")
    {
        sendInterfaceDescriptors();
    }
    else if (command == "sendHidDescriptors")
    {
        sendHidDescriptors();
    }
    else if (command == "sendIADescriptors")
    {
        sendIADescriptors();
    }
    else if (command == "sendEndpointData")
    {
        sendEndpointData();
    }
    else if (command == "sendUnknownDescriptors")
    {
        sendUnknownDescriptors();
    }
    else if (command == "sendDescriptorconfig")
    {
        sendDescriptorconfig();
    }
    else if (command == "YIELD")
    {
        enableYield = true;
    }
    else if (command == "YIELD_END")
    {
        enableYield = false;
    }
    else
    {
        tx_Que("Unknown command received: %s\n", command.c_str());
    }
}
