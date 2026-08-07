#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace makcm {
namespace hid {

struct ReportField {
    bool present = false;
    uint16_t bitOffset = 0;
    uint8_t bitSize = 0;
    int32_t logicalMinimum = 0;
    int32_t logicalMaximum = 0;
};

struct MouseReportLayout {
    bool valid = false;
    bool hasReportId = false;
    uint8_t reportId = 0;
    uint16_t reportLength = 0;
    ReportField buttons[5] = {};
    ReportField x = {};
    ReportField y = {};
    ReportField wheel = {};
    ReportField pan = {};
};

namespace detail {

inline uint8_t itemSize(uint8_t prefix) {
    const uint8_t encoded = prefix & 0x03;
    return encoded == 3 ? 4 : encoded;
}

inline uint32_t unsignedValue(const uint8_t *data, uint8_t size) {
    uint32_t value = 0;
    for (uint8_t i = 0; i < size; ++i) {
        value |= static_cast<uint32_t>(data[i]) << (8 * i);
    }
    return value;
}

inline int32_t signedValue(const uint8_t *data, uint8_t size) {
    const uint32_t value = unsignedValue(data, size);
    if (size == 0 || size >= 4) {
        return static_cast<int32_t>(value);
    }
    const uint8_t bits = static_cast<uint8_t>(size * 8);
    const uint32_t sign = static_cast<uint32_t>(1) << (bits - 1);
    return static_cast<int32_t>((value ^ sign) - sign);
}

inline void clearLocal(uint32_t *usages, uint8_t &usageCount,
                       bool &hasUsageRange, uint32_t &usageMinimum,
                       uint32_t &usageMaximum) {
    memset(usages, 0, sizeof(uint32_t) * 32);
    usageCount = 0;
    hasUsageRange = false;
    usageMinimum = 0;
    usageMaximum = 0;
}

inline uint32_t localUsage(const uint32_t *usages, uint8_t usageCount,
                           bool hasUsageRange, uint32_t usageMinimum,
                           uint32_t usageMaximum, uint16_t index) {
    if (index < usageCount) {
        return usages[index];
    }
    if (hasUsageRange) {
        const uint32_t candidate = usageMinimum + index;
        return candidate <= usageMaximum ? candidate : usageMaximum;
    }
    return 0;
}

inline MouseReportLayout *candidateFor(MouseReportLayout *candidates,
                                        uint8_t &count, uint8_t reportId) {
    for (uint8_t i = 0; i < count; ++i) {
        if (candidates[i].reportId == reportId) {
            return &candidates[i];
        }
    }
    if (count >= 8) {
        return nullptr;
    }
    MouseReportLayout *candidate = &candidates[count++];
    candidate->reportId = reportId;
    candidate->hasReportId = reportId != 0;
    return candidate;
}

inline void assignField(ReportField &field, uint16_t bitOffset,
                        uint8_t bitSize, int32_t logicalMinimum,
                        int32_t logicalMaximum) {
    if (field.present || bitSize == 0 || bitSize > 32) {
        return;
    }
    field.present = true;
    field.bitOffset = bitOffset;
    field.bitSize = bitSize;
    field.logicalMinimum = logicalMinimum;
    field.logicalMaximum = logicalMaximum;
}

} // namespace detail

inline bool parseMouseReportDescriptor(const uint8_t *descriptor,
                                       size_t length,
                                       MouseReportLayout &layout) {
    layout = {};
    if (descriptor == nullptr || length == 0) {
        return false;
    }

    struct GlobalState {
        uint32_t usagePage = 0;
        int32_t logicalMinimum = 0;
        int32_t logicalMaximum = 0;
        uint32_t reportSize = 0;
        uint32_t reportCount = 0;
        uint8_t reportId = 0;
    } global;
    GlobalState stack[4] = {};
    uint8_t stackDepth = 0;
    uint16_t inputBitOffsets[256] = {};
    MouseReportLayout candidates[8] = {};
    uint8_t candidateCount = 0;

    uint32_t usages[32] = {};
    uint8_t usageCount = 0;
    bool hasUsageRange = false;
    uint32_t usageMinimum = 0;
    uint32_t usageMaximum = 0;
    uint8_t collectionDepth = 0;
    int16_t mouseApplicationDepth = -1;

    for (size_t offset = 0; offset < length;) {
        const uint8_t prefix = descriptor[offset++];
        if (prefix == 0xFE) {
            if (offset + 2 > length) return false;
            const uint8_t longLength = descriptor[offset];
            offset += 2;
            if (offset + longLength > length) return false;
            offset += longLength;
            continue;
        }
        const uint8_t size = detail::itemSize(prefix);
        if (offset + size > length) return false;
        const uint8_t type = (prefix >> 2) & 0x03;
        const uint8_t tag = (prefix >> 4) & 0x0F;
        const uint8_t *data = descriptor + offset;
        const uint32_t value = detail::unsignedValue(data, size);
        offset += size;

        if (type == 1) { // Global
            switch (tag) {
            case 0x0: global.usagePage = value; break;
            case 0x1: global.logicalMinimum = detail::signedValue(data, size); break;
            case 0x2:
                global.logicalMaximum = global.logicalMinimum < 0
                    ? detail::signedValue(data, size)
                    : static_cast<int32_t>(value);
                break;
            case 0x7: global.reportSize = value; break;
            case 0x8: global.reportId = static_cast<uint8_t>(value); break;
            case 0x9: global.reportCount = value; break;
            case 0xA:
                if (stackDepth < 4) stack[stackDepth++] = global;
                break;
            case 0xB:
                if (stackDepth != 0) global = stack[--stackDepth];
                break;
            default: break;
            }
            continue;
        }

        if (type == 2) { // Local
            uint32_t qualified = value;
            if (size < 4) qualified |= global.usagePage << 16;
            if (tag == 0x0 && usageCount < 32) {
                usages[usageCount++] = qualified;
            } else if (tag == 0x1) {
                usageMinimum = qualified;
                hasUsageRange = true;
            } else if (tag == 0x2) {
                usageMaximum = qualified;
                hasUsageRange = true;
            }
            continue;
        }

        if (type != 0) continue;

        if (tag == 0xA) { // Collection
            const uint32_t usage = detail::localUsage(
                usages, usageCount, hasUsageRange, usageMinimum, usageMaximum, 0);
            ++collectionDepth;
            if (value == 0x01 && (usage >> 16) == 0x01 &&
                (usage & 0xFFFF) == 0x02) {
                mouseApplicationDepth = collectionDepth;
            }
            detail::clearLocal(usages, usageCount, hasUsageRange,
                               usageMinimum, usageMaximum);
            continue;
        }
        if (tag == 0xC) { // End Collection
            if (mouseApplicationDepth == collectionDepth) {
                mouseApplicationDepth = -1;
            }
            if (collectionDepth != 0) --collectionDepth;
            detail::clearLocal(usages, usageCount, hasUsageRange,
                               usageMinimum, usageMaximum);
            continue;
        }

        if (tag == 0x8) { // Input
            const uint32_t totalBits = global.reportSize * global.reportCount;
            const uint16_t firstBit = inputBitOffsets[global.reportId];
            const bool variable = (value & 0x02) != 0;
            const bool relative = (value & 0x04) != 0;
            if (mouseApplicationDepth >= 0 && variable &&
                global.reportSize <= 32 && global.reportCount <= 64) {
                MouseReportLayout *candidate = detail::candidateFor(
                    candidates, candidateCount, global.reportId);
                if (candidate != nullptr) {
                    for (uint32_t i = 0; i < global.reportCount; ++i) {
                        const uint32_t usage = detail::localUsage(
                            usages, usageCount, hasUsageRange,
                            usageMinimum, usageMaximum, i);
                        const uint16_t bitOffset = static_cast<uint16_t>(
                            firstBit + i * global.reportSize);
                        const uint16_t page = usage >> 16;
                        const uint16_t id = usage & 0xFFFF;
                        if (page == 0x09 && id >= 1 && id <= 5) {
                            detail::assignField(candidate->buttons[id - 1],
                                bitOffset, global.reportSize,
                                global.logicalMinimum, global.logicalMaximum);
                        } else if (relative && page == 0x01 && id == 0x30) {
                            detail::assignField(candidate->x, bitOffset,
                                global.reportSize, global.logicalMinimum,
                                global.logicalMaximum);
                        } else if (relative && page == 0x01 && id == 0x31) {
                            detail::assignField(candidate->y, bitOffset,
                                global.reportSize, global.logicalMinimum,
                                global.logicalMaximum);
                        } else if (relative && page == 0x01 && id == 0x38) {
                            detail::assignField(candidate->wheel, bitOffset,
                                global.reportSize, global.logicalMinimum,
                                global.logicalMaximum);
                        } else if (relative && page == 0x0C && id == 0x0238) {
                            detail::assignField(candidate->pan, bitOffset,
                                global.reportSize, global.logicalMinimum,
                                global.logicalMaximum);
                        }
                    }
                }
            }
            if (totalBits > static_cast<uint32_t>(UINT16_MAX - firstBit)) {
                return false;
            }
            inputBitOffsets[global.reportId] =
                static_cast<uint16_t>(firstBit + totalBits);
        }
        detail::clearLocal(usages, usageCount, hasUsageRange,
                           usageMinimum, usageMaximum);
    }

    for (uint8_t i = 0; i < candidateCount; ++i) {
        MouseReportLayout &candidate = candidates[i];
        if (!candidate.x.present || !candidate.y.present) continue;
        candidate.reportLength = static_cast<uint16_t>(
            (inputBitOffsets[candidate.reportId] + 7) / 8 +
            (candidate.hasReportId ? 1 : 0));
        if (candidate.reportLength == 0 || candidate.reportLength > 64) continue;
        candidate.valid = true;
        layout = candidate;
        return true;
    }
    return false;
}

inline MouseReportLayout bootMouseLayout() {
    MouseReportLayout layout;
    layout.valid = true;
    layout.reportLength = 4;
    for (uint8_t i = 0; i < 5; ++i) {
        detail::assignField(layout.buttons[i], i, 1, 0, 1);
    }
    detail::assignField(layout.x, 8, 8, -127, 127);
    detail::assignField(layout.y, 16, 8, -127, 127);
    detail::assignField(layout.wheel, 24, 8, -127, 127);
    return layout;
}

inline uint32_t readBits(const uint8_t *data, uint16_t length,
                         uint16_t bitOffset, uint8_t bitSize) {
    if (data == nullptr || bitSize == 0 || bitSize > 32 ||
        static_cast<uint32_t>(bitOffset) + bitSize >
            static_cast<uint32_t>(length) * 8) {
        return 0;
    }
    uint32_t value = 0;
    for (uint8_t bit = 0; bit < bitSize; ++bit) {
        const uint16_t source = bitOffset + bit;
        if ((data[source / 8] & (1u << (source % 8))) != 0) {
            value |= static_cast<uint32_t>(1) << bit;
        }
    }
    return value;
}

inline bool writeBits(uint8_t *data, uint16_t length, uint16_t bitOffset,
                      uint8_t bitSize, uint32_t value) {
    if (data == nullptr || bitSize == 0 || bitSize > 32 ||
        static_cast<uint32_t>(bitOffset) + bitSize >
            static_cast<uint32_t>(length) * 8) {
        return false;
    }
    for (uint8_t bit = 0; bit < bitSize; ++bit) {
        const uint16_t target = bitOffset + bit;
        const uint8_t mask = static_cast<uint8_t>(1u << (target % 8));
        if ((value & (static_cast<uint32_t>(1) << bit)) != 0) {
            data[target / 8] |= mask;
        } else {
            data[target / 8] &= static_cast<uint8_t>(~mask);
        }
    }
    return true;
}

inline int32_t clampToField(int32_t value, const ReportField &field,
                            int32_t fallbackMinimum = -127,
                            int32_t fallbackMaximum = 127) {
    const int32_t minimum = field.present && field.logicalMinimum < 0
        ? field.logicalMinimum : fallbackMinimum;
    const int32_t maximum = field.present && field.logicalMaximum > 0
        ? field.logicalMaximum : fallbackMaximum;
    return value < minimum ? minimum : (value > maximum ? maximum : value);
}

inline bool matchesReport(const MouseReportLayout &layout,
                          const uint8_t *report, uint16_t length) {
    return report != nullptr && length >= layout.reportLength &&
        (!layout.hasReportId || report[0] == layout.reportId);
}

inline uint8_t readMouseButtons(const MouseReportLayout &layout,
                                const uint8_t *report, uint16_t length) {
    if (!layout.valid || !matchesReport(layout, report, length)) return 0;
    const uint8_t prefix = layout.hasReportId ? 1 : 0;
    const uint8_t *payload = report + prefix;
    const uint16_t payloadLength = length - prefix;
    uint8_t buttons = 0;
    for (uint8_t i = 0; i < 5; ++i) {
        if (layout.buttons[i].present &&
            readBits(payload, payloadLength, layout.buttons[i].bitOffset,
                     layout.buttons[i].bitSize) != 0) {
            buttons |= static_cast<uint8_t>(1u << i);
        }
    }
    return buttons;
}

inline int32_t signExtend(uint32_t value, uint8_t bitSize) {
    if (bitSize == 0 || bitSize >= 32) {
        return static_cast<int32_t>(value);
    }
    const uint32_t sign = static_cast<uint32_t>(1) << (bitSize - 1);
    const uint32_t mask = (static_cast<uint32_t>(1) << bitSize) - 1;
    value &= mask;
    return static_cast<int32_t>((value ^ sign) - sign);
}

inline int32_t readFieldValue(const uint8_t *payload,
                              uint16_t payloadLength,
                              const ReportField &field) {
    if (!field.present) return 0;
    const uint32_t raw = readBits(payload, payloadLength, field.bitOffset,
                                  field.bitSize);
    return field.logicalMinimum < 0
        ? signExtend(raw, field.bitSize)
        : static_cast<int32_t>(raw);
}

struct MouseMergeDelta {
    int64_t x = 0;
    int64_t y = 0;
    int64_t wheel = 0;
    int64_t pan = 0;
};

inline int64_t clamp64(int64_t value, int64_t minimum, int64_t maximum) {
    return value < minimum ? minimum : (value > maximum ? maximum : value);
}

// Merge pending relative injection into a real mouse report, or generate a
// synthetic report from the last real report.  pending* contains relative
// integer deltas, never absolute coordinates.  Any value that does not fit in
// the current HID field remains pending for the next USB poll.
inline uint16_t mergeMouseReport(const MouseReportLayout &layout,
                                 const uint8_t *baseReport,
                                 uint16_t baseLength,
                                 bool includeBaseAxes,
                                 uint8_t injectedButtons,
                                 int64_t &pendingX,
                                 int64_t &pendingY,
                                 int64_t &pendingWheel,
                                 int64_t &pendingPan,
                                 uint8_t *output,
                                 uint16_t capacity,
                                 MouseMergeDelta *consumed = nullptr) {
    if (!layout.valid || output == nullptr || layout.reportLength == 0) {
        return 0;
    }
    const bool hasBase = matchesReport(layout, baseReport, baseLength);
    if (includeBaseAxes && !hasBase) return 0;
    const uint16_t outputLength = hasBase ? baseLength : layout.reportLength;
    if (outputLength < layout.reportLength || outputLength > capacity) return 0;

    memset(output, 0, outputLength);
    if (hasBase) memcpy(output, baseReport, outputLength);
    if (layout.hasReportId) output[0] = layout.reportId;

    const uint8_t prefix = layout.hasReportId ? 1 : 0;
    uint8_t *payload = output + prefix;
    const uint16_t payloadLength = outputLength - prefix;
    const uint8_t *basePayload = hasBase ? baseReport + prefix : nullptr;
    const uint16_t basePayloadLength = hasBase ? baseLength - prefix : 0;

    MouseMergeDelta localConsumed;
    const auto mergeField = [basePayload, basePayloadLength, includeBaseAxes,
                             payload, payloadLength](
        const ReportField &field, int64_t &pending,
        int32_t fallbackMinimum, int32_t fallbackMaximum,
        int64_t &consumedValue) {
        if (!field.present) return;
        const int64_t physical = includeBaseAxes
            ? readFieldValue(basePayload, basePayloadLength, field) : 0;
        const int64_t minimum = field.logicalMinimum < 0
            ? field.logicalMinimum : fallbackMinimum;
        const int64_t maximum = field.logicalMaximum > 0
            ? field.logicalMaximum : fallbackMaximum;
        // A malformed/out-of-range physical field must never manufacture an
        // injected contribution. Preserve the base bits and leave the entire
        // relative delta pending for a later valid report.
        if (includeBaseAxes && (physical < minimum || physical > maximum)) {
            consumedValue = 0;
            return;
        }
        // Clamp the injected contribution against the remaining field range.
        // This avoids overflowing int64 even after a very long producer
        // backlog and keeps physical + injected integer sums exact.
        consumedValue = clamp64(pending, minimum - physical,
                                 maximum - physical);
        const int64_t emitted = physical + consumedValue;
        pending -= consumedValue;
        writeBits(payload, payloadLength, field.bitOffset, field.bitSize,
                  static_cast<uint32_t>(emitted));
    };

    mergeField(layout.x, pendingX, -127, 127, localConsumed.x);
    mergeField(layout.y, pendingY, -127, 127, localConsumed.y);
    mergeField(layout.wheel, pendingWheel, -127, 127,
               localConsumed.wheel);
    mergeField(layout.pan, pendingPan, -127, 127, localConsumed.pan);

    const uint8_t physicalButtons = hasBase
        ? readMouseButtons(layout, baseReport, baseLength) : 0;
    const uint8_t buttons = physicalButtons | injectedButtons;
    for (uint8_t i = 0; i < 5; ++i) {
        if (layout.buttons[i].present) {
            writeBits(payload, payloadLength, layout.buttons[i].bitOffset,
                      layout.buttons[i].bitSize,
                      (buttons & (1u << i)) != 0 ? 1 : 0);
        }
    }
    if (consumed != nullptr) *consumed = localConsumed;
    return outputLength;
}

inline uint16_t composeMouseReport(const MouseReportLayout &layout,
                                   const uint8_t *lastReport,
                                   uint16_t lastLength,
                                   uint8_t injectedButtons,
                                   int32_t x, int32_t y,
                                   int32_t wheel, int32_t pan,
                                   uint8_t *output, uint16_t capacity) {
    if (!layout.valid || output == nullptr ||
        capacity < layout.reportLength || layout.reportLength == 0) {
        return 0;
    }
    memset(output, 0, layout.reportLength);
    if (matchesReport(layout, lastReport, lastLength)) {
        memcpy(output, lastReport, layout.reportLength);
    }
    const uint8_t physicalButtons =
        readMouseButtons(layout, lastReport, lastLength);
    if (layout.hasReportId) output[0] = layout.reportId;
    const uint8_t prefix = layout.hasReportId ? 1 : 0;
    uint8_t *payload = output + prefix;
    const uint16_t payloadLength = layout.reportLength - prefix;

    const auto writeField = [payload, payloadLength](
        const ReportField &field, int32_t value) {
        if (!field.present) return;
        const int32_t clamped = clampToField(value, field);
        writeBits(payload, payloadLength, field.bitOffset, field.bitSize,
                  static_cast<uint32_t>(clamped));
    };
    writeField(layout.x, x);
    writeField(layout.y, y);
    writeField(layout.wheel, wheel);
    writeField(layout.pan, pan);

    const uint8_t buttons = physicalButtons | injectedButtons;
    for (uint8_t i = 0; i < 5; ++i) {
        if (layout.buttons[i].present) {
            writeBits(payload, payloadLength, layout.buttons[i].bitOffset,
                      layout.buttons[i].bitSize,
                      (buttons & (1u << i)) != 0 ? 1 : 0);
        }
    }
    return layout.reportLength;
}

} // namespace hid
} // namespace makcm
