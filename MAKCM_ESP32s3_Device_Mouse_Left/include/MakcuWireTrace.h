#pragma once

#include <Arduino.h>
#include <cstddef>
#include <cstdint>

namespace MakcuWireTrace {

enum class Direction : uint8_t {
    Rx = 0,
    Tx = 1,
};

// Starts an empty capture. The auto-start variant is used when the first byte
// of Makcu's baud-rate handshake arrives, before Secondary owns the fast port.
void start(bool automatic = false);
void stop();
bool active();

// Store exact wire bytes as timestamped chunks. Chunk boundaries are an
// implementation detail; concatenating adjacent records of the same direction
// reconstructs the byte stream exactly.
void record(Direction direction, const uint8_t *data, size_t length);

void printStatus(Print &out);
void dumpHex(Print &out);

} // namespace MakcuWireTrace
