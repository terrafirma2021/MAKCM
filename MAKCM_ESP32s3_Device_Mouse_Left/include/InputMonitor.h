#pragma once

#include <Arduino.h>
#include <stdint.h>

// Maps physical inputs to the byte stream consumed by SecondaryAim:
// bit0 = fire/LMB/R2, bit1 = ADS/RMB/L2, bits2..4 = mouse M/S4/S5.
void beginInputMonitor();
void resetInputMonitor();
void updateDseTriggerMonitor(const uint8_t *report, uint16_t length);
void updateXboxTriggerMonitor(const uint8_t *report, uint16_t length);
void updateMouseButtonMonitor(const uint8_t *report, uint16_t length,
                              bool reportIdPrefix);
void observeRightMouseCommand(const char *command);

void handleKmVersionCommand(const char *command);
void handleKmEchoCommand(const char *command);
void handleKmButtonsCommand(const char *command);

// Consumes the CH343 speed-switch blob used by MakcuDevice. Returns true when
// the byte belongs to the blob and must not enter the ASCII command parser.
bool consumeMakcuSpeedSwitchByte(uint8_t byte);
