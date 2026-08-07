#include "MakcuWireTrace.h"

#include <atomic>
#include <cstring>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>

namespace MakcuWireTrace {
namespace {

// A typical km.move line is 14-20 bytes. This holds roughly 6-8 seconds at
// 1000 commands/s, including km.buttons traffic and record headers.
constexpr size_t kCapacity = 128 * 1024;
constexpr uint32_t kDumpVersion = 1;

struct __attribute__((packed)) RecordHeader {
    uint32_t timestampUs;
    uint16_t length;
    uint8_t direction;
    uint8_t reserved;
};

static uint8_t captureBuffer[kCapacity];
static size_t captureUsed = 0;
static uint32_t captureRecords = 0;
static uint32_t droppedBytes = 0;
static uint64_t captureStartUs = 0;
static bool overflowed = false;
static bool automaticStart = false;
static bool automaticCaptureConsumed = false;
static std::atomic<bool> captureActive{false};
static portMUX_TYPE captureMux = portMUX_INITIALIZER_UNLOCKED;

uint32_t crc32(const uint8_t *data, size_t length) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u &
                static_cast<uint32_t>(-static_cast<int32_t>(crc & 1u)));
        }
    }
    return ~crc;
}

} // namespace

void start(bool automatic) {
    portENTER_CRITICAL(&captureMux);
    // Preserve the first automatic capture even if Secondary reconnects after
    // the buffer fills. A manual BRIDGE_TRACE_START can still begin a new one.
    if (automatic && automaticCaptureConsumed) {
        portEXIT_CRITICAL(&captureMux);
        return;
    }
    if (automatic) {
        automaticCaptureConsumed = true;
    }
    captureUsed = 0;
    captureRecords = 0;
    droppedBytes = 0;
    overflowed = false;
    automaticStart = automatic;
    captureStartUs = static_cast<uint64_t>(esp_timer_get_time());
    captureActive.store(true, std::memory_order_release);
    portEXIT_CRITICAL(&captureMux);
}

void stop() {
    portENTER_CRITICAL(&captureMux);
    captureActive.store(false, std::memory_order_release);
    portEXIT_CRITICAL(&captureMux);
}

bool active() {
    return captureActive.load(std::memory_order_acquire);
}

void record(Direction direction, const uint8_t *data, size_t length) {
    if (!active() || data == nullptr || length == 0) {
        return;
    }

    while (length != 0 && active()) {
        const uint16_t chunkLength = static_cast<uint16_t>(
            length > UINT16_MAX ? UINT16_MAX : length);
        const RecordHeader header = {
            static_cast<uint32_t>(
                static_cast<uint64_t>(esp_timer_get_time()) - captureStartUs),
            chunkLength,
            static_cast<uint8_t>(direction),
            0,
        };

        portENTER_CRITICAL(&captureMux);
        if (!captureActive.load(std::memory_order_relaxed)) {
            portEXIT_CRITICAL(&captureMux);
            return;
        }
        const size_t required = sizeof(header) + chunkLength;
        if (captureUsed + required > kCapacity) {
            overflowed = true;
            droppedBytes += static_cast<uint32_t>(length);
            captureActive.store(false, std::memory_order_release);
            portEXIT_CRITICAL(&captureMux);
            return;
        }
        memcpy(captureBuffer + captureUsed, &header, sizeof(header));
        captureUsed += sizeof(header);
        memcpy(captureBuffer + captureUsed, data, chunkLength);
        captureUsed += chunkLength;
        ++captureRecords;
        portEXIT_CRITICAL(&captureMux);

        data += chunkLength;
        length -= chunkLength;
    }
}

void printStatus(Print &out) {
    bool isActive;
    bool didOverflow;
    bool wasAutomatic;
    size_t used;
    uint32_t records;
    uint32_t dropped;
    portENTER_CRITICAL(&captureMux);
    isActive = captureActive.load(std::memory_order_relaxed);
    didOverflow = overflowed;
    wasAutomatic = automaticStart;
    used = captureUsed;
    records = captureRecords;
    dropped = droppedBytes;
    portEXIT_CRITICAL(&captureMux);
    out.printf(
        "BRIDGE_TRACE_STATUS active=%u bytes=%u capacity=%u records=%lu "
        "overflow=%u dropped=%lu auto=%u\r\n",
        isActive ? 1u : 0u, static_cast<unsigned>(used),
        static_cast<unsigned>(kCapacity), static_cast<unsigned long>(records),
        didOverflow ? 1u : 0u, static_cast<unsigned long>(dropped),
        wasAutomatic ? 1u : 0u);
}

void dumpHex(Print &out) {
    stop();

    const size_t used = captureUsed;
    const uint32_t records = captureRecords;
    const uint32_t dropped = droppedBytes;
    const bool didOverflow = overflowed;
    const bool wasAutomatic = automaticStart;
    const uint32_t checksum = crc32(captureBuffer, used);
    out.printf(
        "BRIDGE_TRACE_BEGIN version=%lu bytes=%u records=%lu overflow=%u "
        "dropped=%lu auto=%u crc32=%08lX\r\n",
        static_cast<unsigned long>(kDumpVersion), static_cast<unsigned>(used),
        static_cast<unsigned long>(records), didOverflow ? 1u : 0u,
        static_cast<unsigned long>(dropped), wasAutomatic ? 1u : 0u,
        static_cast<unsigned long>(checksum));

    static constexpr char kHex[] = "0123456789ABCDEF";
    char line[129];
    size_t offset = 0;
    while (offset < used) {
        const size_t count = (used - offset) > 64 ? 64 : (used - offset);
        for (size_t i = 0; i < count; ++i) {
            const uint8_t value = captureBuffer[offset + i];
            line[i * 2] = kHex[value >> 4];
            line[i * 2 + 1] = kHex[value & 0x0F];
        }
        line[count * 2] = '\0';
        out.println(line);
        offset += count;
    }
    out.println("BRIDGE_TRACE_END");
}

} // namespace MakcuWireTrace
