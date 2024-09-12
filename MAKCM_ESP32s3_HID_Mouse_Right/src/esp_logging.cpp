#include "EspUsbHost.h"
#include <stdarg.h>
#include <stdio.h>
#include <sstream>
#include "freertos/semphr.h"

RingBuf<char, 620> txBuffer;
RingBuf<char, 512> logBuffer;
RingBuf<char, 512> rawBytesBuffer;

int EspUsbHost::current_log_level = LOG_LEVEL_OFF;
SemaphoreHandle_t serialMutex = xSemaphoreCreateMutex();
const char *currentSerialOwner = nullptr;

bool isMutexLocked = false;
const char *lockOwner = nullptr;

bool EspUsbHost::lockMutex(const char *owner)
{
    if (xSemaphoreTake(serialMutex, (TickType_t)100) == pdTRUE)
    {
        if (!isMutexLocked)
        {
            isMutexLocked = true;
            lockOwner = owner;
            Serial1.printf("Mutex locked by %s.\n", owner);
            xSemaphoreGive(serialMutex);
            return true;
        }
        xSemaphoreGive(serialMutex);
    }
    Serial1.println("Error: Mutex already locked or failed to acquire.");
    return false;
}

void EspUsbHost::unlockMutex(const char *owner)
{
    if (xSemaphoreTake(serialMutex, (TickType_t)100) == pdTRUE)
    {
        if (isMutexLocked && strcmp(lockOwner, owner) == 0)
        {
            isMutexLocked = false;
            lockOwner = nullptr;
            Serial1.printf("Mutex unlocked by %s.\n", owner);
        }
        xSemaphoreGive(serialMutex);
    }
}

void EspUsbHost::log(int level, const char *format, ...)
{
    if (level > current_log_level || current_log_level == LOG_LEVEL_OFF)
    {
        return;
    }

    if (isMutexLocked && strcmp(lockOwner, "log") != 0)
    {
        return;
    }

    if (xSemaphoreTake(serialMutex, (TickType_t)100) == pdTRUE)
    {
        currentSerialOwner = "log";

        char logMsg[512];
        va_list args;
        va_start(args, format);
        vsnprintf(logMsg, sizeof(logMsg), format, args);
        va_end(args);

        for (int i = 0; i < strlen(logMsg); ++i)
        {
            if (!logBuffer.isFull())
            {
                logBuffer.push(logMsg[i]);
            }
            else
            {
                Serial1.println("Error: Log buffer overflow detected.");
                break;
            }
        }

        while (!logBuffer.isEmpty())
        {
            char byte;
            logBuffer.pop(byte);
            Serial1.write(byte);
        }

        xSemaphoreGive(serialMutex);
        currentSerialOwner = nullptr;
    }
    else
    {
        Serial1.println("Error: Failed to acquire Serial1 in log.");
    }
}

void EspUsbHost::logRawBytes(const uint8_t *data, size_t length, const std::string &label)
{
    if (current_log_level < LOG_LEVEL_FIXED)
    {
        return;
    }

    if (isMutexLocked && strcmp(lockOwner, "logRawBytes") != 0)
    {
        return;
    }

    if (xSemaphoreTake(serialMutex, (TickType_t)100) == pdTRUE)
    {
        currentSerialOwner = "logRawBytes";

        char logMsg[512];
        int logIndex = snprintf(logMsg, sizeof(logMsg), "**%s**\n", label.c_str());

        for (size_t i = 0; i < length && logIndex < sizeof(logMsg) - 3; ++i)
        {
            logIndex += snprintf(logMsg + logIndex, sizeof(logMsg) - logIndex, "%02X ", data[i]);
        }

        logIndex += snprintf(logMsg + logIndex, sizeof(logMsg) - logIndex, "\n");

        for (int i = 0; i < strlen(logMsg); ++i)
        {
            if (!rawBytesBuffer.isFull())
            {
                rawBytesBuffer.push(logMsg[i]);
            }
            else
            {
                Serial1.println("Error: Raw byte buffer overflow detected.");
                break;
            }
        }

        while (!rawBytesBuffer.isEmpty())
        {
            char byte;
            rawBytesBuffer.pop(byte);
            Serial1.write(byte);
        }

        xSemaphoreGive(serialMutex);
        currentSerialOwner = nullptr;
    }
    else
    {
        Serial1.println("Error: Failed to acquire Serial1 in logRawBytes.");
    }
}

bool EspUsbHost::serial1Send(const char *format, ...)
{
    if (isMutexLocked && strcmp(lockOwner, "serial1Send") != 0)
    {
        return false;
    }

    if (xSemaphoreTake(serialMutex, (TickType_t)100) == pdTRUE)
    {
        currentSerialOwner = "serial1Send";

        char logMsg[620];
        va_list args;
        va_start(args, format);
        vsnprintf(logMsg, sizeof(logMsg), format, args);
        va_end(args);

        for (int i = 0; i < strlen(logMsg); ++i)
        {
            if (!txBuffer.isFull())
            {
                txBuffer.push(logMsg[i]);
            }
            else
            {
                Serial1.println("Error: TX buffer overflow detected.");
                xSemaphoreGive(serialMutex);
                currentSerialOwner = nullptr;
                return false;
            }
        }

        while (!txBuffer.isEmpty())
        {
            char byte;
            txBuffer.pop(byte);
            Serial1.write(byte);
        }

        xSemaphoreGive(serialMutex);
        currentSerialOwner = nullptr;
        return true;
    }
    else
    {
        Serial1.println("Error: Failed to acquire Serial1 in serial1Send.");
        return false;
    }
}
