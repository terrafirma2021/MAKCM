#pragma once

#include <Arduino.h>
#include "BridgeProtocol.h"

void beginBridgeTransport();
void serviceBridgeTransport();
bool consumeBridgeSerialByte(uint8_t byte);
bool sendBridgeFrame(makcm::bridge::MessageType type,
                     const uint8_t *payload,
                     uint16_t length);
bool sendBridgeFrameForSession(makcm::bridge::MessageType type,
                               const uint8_t *payload, uint16_t length,
                               uint32_t session);
bool sendBridgeText(const char *text, bool appendNewline = true);

uint32_t bridgeRxFrameCount();
uint32_t bridgeRxErrorCount();
uint32_t bridgeActiveSession();
uint8_t bridgeProxyState();
