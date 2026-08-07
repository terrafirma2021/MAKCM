#pragma once

// Platform-independent protocol primitives for the direct-stick control
// plane.  The left board deliberately keeps this parser independent from
// UART/FreeRTOS code so that malformed commands can be rejected identically
// on a host and on the target.

#include <stdint.h>

#include <array>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <string_view>

namespace makcm::stick {

constexpr uint16_t kDefaultTtlMs = 32;
constexpr uint16_t kMinTtlMs = 16;
// 80 ms is the protocol fail-closed ceiling.  A command producer may use the
// 32 ms default (roughly four 144 Hz ticks), but cannot ask the board to hold
// synthetic state beyond this bound.
constexpr uint16_t kMaxTtlMs = 80;

enum class StickSource : uint8_t {
    None = 0,
    StateQ15 = 1,
    Rate = 2,
    MoveCompat = 3,
};

enum class StickCommandKind : uint8_t {
    State = 0,
    Rate = 1,
    Lease = 2,
};

// A command contains no implicit host send timestamp.  acceptedAtMs is
// assigned only after all validation succeeds on the board; this makes TTL
// fail closed when a stale or duplicate command is received.
struct StickCommand {
    StickCommandKind kind = StickCommandKind::State;
    StickSource source = StickSource::StateQ15;
    uint32_t injectionSession = 0;
    uint32_t sequence = 0;
    int32_t x = 0;
    int32_t y = 0;
    uint16_t ttlMs = kDefaultTtlMs;
    // Populated by a binary bridge envelope or by the session caller.  ASCII
    // Secondary commands intentionally do not carry this value: the active
    // physical bridge session is the authority at acceptance time.
    uint32_t physicalSession = 0;
};

enum class StickParseError : uint8_t {
    None = 0,
    NullInput,
    Empty,
    MissingCrc,
    InvalidCrcHex,
    CrcMismatch,
    InvalidCommand,
    InvalidArguments,
    InvalidNumber,
    NumberOutOfRange,
    InvalidTtl,
    InvalidSource,
};

inline uint16_t crc16Ccitt(const uint8_t *data, size_t length) {
    uint16_t crc = 0xFFFF;
    if (data == nullptr && length != 0) return 0;
    for (size_t i = 0; i < length; ++i) {
        crc ^= static_cast<uint16_t>(data[i]) << 8;
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000u)
                      ? static_cast<uint16_t>((crc << 1) ^ 0x1021u)
                      : static_cast<uint16_t>(crc << 1);
        }
    }
    return crc;
}

inline uint16_t crc16Ccitt(std::string_view text) {
    return crc16Ccitt(reinterpret_cast<const uint8_t *>(text.data()),
                      text.size());
}

inline bool parseHex16(std::string_view text, uint16_t &value) {
    if (text.size() != 4) return false;
    uint16_t result = 0;
    for (const char ch : text) {
        uint8_t nibble = 0;
        if (ch >= '0' && ch <= '9') nibble = static_cast<uint8_t>(ch - '0');
        else if (ch >= 'a' && ch <= 'f') nibble = static_cast<uint8_t>(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') nibble = static_cast<uint8_t>(ch - 'A' + 10);
        else return false;
        result = static_cast<uint16_t>((result << 4) | nibble);
    }
    value = result;
    return true;
}

inline bool parseUnsigned(std::string_view text, uint64_t &value) {
    if (text.empty()) return false;
    size_t index = 0;
    int base = 10;
    if (text.size() > 2 && text[0] == '0' &&
        (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        index = 2;
        if (index == text.size()) return false;
    }
    uint64_t result = 0;
    for (; index < text.size(); ++index) {
        const char ch = text[index];
        uint8_t digit = 0;
        if (ch >= '0' && ch <= '9') digit = static_cast<uint8_t>(ch - '0');
        else if (base == 16 && ch >= 'a' && ch <= 'f') digit = static_cast<uint8_t>(ch - 'a' + 10);
        else if (base == 16 && ch >= 'A' && ch <= 'F') digit = static_cast<uint8_t>(ch - 'A' + 10);
        else return false;
        if (digit >= base || result > (std::numeric_limits<uint64_t>::max() - digit) / base)
            return false;
        result = result * static_cast<uint64_t>(base) + digit;
    }
    value = result;
    return true;
}

inline bool parseSigned(std::string_view text, int64_t &value) {
    if (text.empty()) return false;
    bool negative = false;
    if (text.front() == '+' || text.front() == '-') {
        negative = text.front() == '-';
        text.remove_prefix(1);
    }
    uint64_t magnitude = 0;
    if (!parseUnsigned(text, magnitude)) return false;
    constexpr uint64_t kInt64MinMagnitude =
        static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1u;
    if ((!negative && magnitude > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) ||
        (negative && magnitude > kInt64MinMagnitude)) {
        return false;
    }
    if (negative) {
        value = magnitude == kInt64MinMagnitude
                    ? std::numeric_limits<int64_t>::min()
                    : -static_cast<int64_t>(magnitude);
    } else {
        value = static_cast<int64_t>(magnitude);
    }
    return true;
}

inline bool parseU32(std::string_view text, uint32_t &value) {
    uint64_t parsed = 0;
    if (!parseUnsigned(text, parsed) || parsed > std::numeric_limits<uint32_t>::max())
        return false;
    value = static_cast<uint32_t>(parsed);
    return true;
}

inline bool parseI32(std::string_view text, int32_t &value) {
    int64_t parsed = 0;
    if (!parseSigned(text, parsed) || parsed < std::numeric_limits<int32_t>::min() ||
        parsed > std::numeric_limits<int32_t>::max())
        return false;
    value = static_cast<int32_t>(parsed);
    return true;
}

inline bool parseTtl(std::string_view text, uint16_t &value) {
    uint64_t parsed = 0;
    if (!parseUnsigned(text, parsed) || parsed > std::numeric_limits<uint16_t>::max())
        return false;
    if (parsed < kMinTtlMs || parsed > kMaxTtlMs) return false;
    value = static_cast<uint16_t>(parsed);
    return true;
}

inline StickSource sourceForKind(StickCommandKind kind) {
    switch (kind) {
    case StickCommandKind::State: return StickSource::StateQ15;
    case StickCommandKind::Rate: return StickSource::Rate;
    case StickCommandKind::Lease: return StickSource::MoveCompat;
    }
    return StickSource::None;
}

inline bool parseStickCommand(const uint8_t *input, size_t length,
                              StickCommand &command,
                              StickParseError *error = nullptr) {
    auto fail = [&](StickParseError reason) {
        if (error != nullptr) *error = reason;
        return false;
    };
    if (input == nullptr) return fail(StickParseError::NullInput);
    while (length != 0 && (input[length - 1] == '\n' || input[length - 1] == '\r')) --length;
    if (length == 0) return fail(StickParseError::Empty);
    const std::string_view line(reinterpret_cast<const char *>(input), length);
    const size_t star = line.find('*');
    if (star == std::string_view::npos || star == 0 || line.find('*', star + 1) != std::string_view::npos)
        return fail(StickParseError::MissingCrc);
    const std::string_view body = line.substr(0, star);
    uint16_t suppliedCrc = 0;
    if (!parseHex16(line.substr(star + 1), suppliedCrc))
        return fail(StickParseError::InvalidCrcHex);
    if (crc16Ccitt(body) != suppliedCrc) return fail(StickParseError::CrcMismatch);

    constexpr std::string_view kPrefix = "km.stick.";
    if (body.substr(0, kPrefix.size()) != kPrefix)
        return fail(StickParseError::InvalidCommand);
    const size_t open = body.find('(', kPrefix.size());
    if (open == std::string_view::npos || body.back() != ')' || open <= kPrefix.size())
        return fail(StickParseError::InvalidCommand);
    const std::string_view operation = body.substr(kPrefix.size(), open - kPrefix.size());
    StickCommandKind kind;
    if (operation == "state") kind = StickCommandKind::State;
    else if (operation == "rate") kind = StickCommandKind::Rate;
    else if (operation == "lease") kind = StickCommandKind::Lease;
    else return fail(StickParseError::InvalidSource);

    const std::string_view args = body.substr(open + 1, body.size() - open - 2);
    std::array<std::string_view, 5> fields{};
    size_t count = 0;
    size_t begin = 0;
    for (size_t i = 0; i <= args.size(); ++i) {
        if (i != args.size() && args[i] != ',') continue;
        if (count >= fields.size()) return fail(StickParseError::InvalidArguments);
        fields[count++] = args.substr(begin, i - begin);
        begin = i + 1;
    }
    const size_t expected = kind == StickCommandKind::Lease ? 3 : 5;
    if (count != expected) return fail(StickParseError::InvalidArguments);

    StickCommand parsed;
    parsed.kind = kind;
    parsed.source = sourceForKind(kind);
    if (!parseU32(fields[0], parsed.injectionSession) ||
        !parseU32(fields[1], parsed.sequence))
        return fail(StickParseError::InvalidNumber);
    if (kind != StickCommandKind::Lease) {
        if (!parseI32(fields[2], parsed.x) || !parseI32(fields[3], parsed.y))
            return fail(StickParseError::InvalidNumber);
        if (kind == StickCommandKind::State &&
            (parsed.x < -32767 || parsed.x > 32767 || parsed.y < -32767 || parsed.y > 32767))
            return fail(StickParseError::NumberOutOfRange);
        if (!parseTtl(fields[4], parsed.ttlMs)) return fail(StickParseError::InvalidTtl);
    } else {
        parsed.x = 0;
        parsed.y = 0;
        if (!parseTtl(fields[2], parsed.ttlMs)) return fail(StickParseError::InvalidTtl);
    }
    command = parsed;
    if (error != nullptr) *error = StickParseError::None;
    return true;
}

inline bool parseStickCommand(std::string_view input, StickCommand &command,
                              StickParseError *error = nullptr) {
    return parseStickCommand(reinterpret_cast<const uint8_t *>(input.data()),
                             input.size(), command, error);
}

inline bool parseStickCommand(const char *input, size_t length,
                              StickCommand &command,
                              StickParseError *error = nullptr) {
    return parseStickCommand(reinterpret_cast<const uint8_t *>(input), length,
                             command, error);
}

// Returns the complete line including a terminating LF.  The CRC covers the
// command body only, i.e. bytes before '*', as specified by the V1 protocol.
inline size_t encodeStickCommand(const StickCommand &command, char *output,
                                 size_t capacity) {
    if (output == nullptr || capacity == 0 || command.injectionSession == 0) return 0;
    const char *operation = nullptr;
    int count = 0;
    if (command.kind == StickCommandKind::State) {
        if (command.x < -32767 || command.x > 32767 || command.y < -32767 || command.y > 32767)
            return 0;
        operation = "state";
        count = std::snprintf(output, capacity, "km.stick.%s(%lu,%lu,%ld,%ld,%u)", operation,
                              static_cast<unsigned long>(command.injectionSession),
                              static_cast<unsigned long>(command.sequence),
                              static_cast<long>(command.x), static_cast<long>(command.y),
                              static_cast<unsigned>(command.ttlMs));
    } else if (command.kind == StickCommandKind::Rate) {
        operation = "rate";
        count = std::snprintf(output, capacity, "km.stick.%s(%lu,%lu,%ld,%ld,%u)", operation,
                              static_cast<unsigned long>(command.injectionSession),
                              static_cast<unsigned long>(command.sequence),
                              static_cast<long>(command.x), static_cast<long>(command.y),
                              static_cast<unsigned>(command.ttlMs));
    } else if (command.kind == StickCommandKind::Lease) {
        operation = "lease";
        count = std::snprintf(output, capacity, "km.stick.%s(%lu,%lu,%u)", operation,
                              static_cast<unsigned long>(command.injectionSession),
                              static_cast<unsigned long>(command.sequence),
                              static_cast<unsigned>(command.ttlMs));
    } else {
        return 0;
    }
    if (count < 0 || static_cast<size_t>(count) + 6 > capacity ||
        command.ttlMs < kMinTtlMs || command.ttlMs > kMaxTtlMs)
        return 0;
    const uint16_t crc = crc16Ccitt(reinterpret_cast<const uint8_t *>(output),
                                    static_cast<size_t>(count));
    const int suffix = std::snprintf(output + count, capacity - static_cast<size_t>(count),
                                     "*%04X\n", crc);
    if (suffix < 0 || static_cast<size_t>(count + suffix) >= capacity) return 0;
    return static_cast<size_t>(count + suffix);
}

inline size_t encodeStickState(uint32_t injectionSession, uint32_t sequence,
                               int16_t xQ15, int16_t yQ15, uint16_t ttlMs,
                               char *output, size_t capacity) {
    StickCommand command;
    command.kind = StickCommandKind::State;
    command.source = StickSource::StateQ15;
    command.injectionSession = injectionSession;
    command.sequence = sequence;
    command.x = xQ15;
    command.y = yQ15;
    command.ttlMs = ttlMs;
    return encodeStickCommand(command, output, capacity);
}

inline size_t encodeStickRate(uint32_t injectionSession, uint32_t sequence,
                              int32_t xMdegPerSecond, int32_t yMdegPerSecond,
                              uint16_t ttlMs, char *output, size_t capacity) {
    StickCommand command;
    command.kind = StickCommandKind::Rate;
    command.source = StickSource::Rate;
    command.injectionSession = injectionSession;
    command.sequence = sequence;
    command.x = xMdegPerSecond;
    command.y = yMdegPerSecond;
    command.ttlMs = ttlMs;
    return encodeStickCommand(command, output, capacity);
}

inline size_t encodeStickLease(uint32_t injectionSession, uint32_t sequence,
                               uint16_t ttlMs, char *output, size_t capacity) {
    StickCommand command;
    command.kind = StickCommandKind::Lease;
    command.source = StickSource::MoveCompat;
    command.injectionSession = injectionSession;
    command.sequence = sequence;
    command.ttlMs = ttlMs;
    return encodeStickCommand(command, output, capacity);
}

enum class SequenceDisposition : uint8_t {
    First = 0,
    New = 1,
    NewWithGap = 2,
    Duplicate = 3,
    Stale = 4,
};

struct SequenceCheck {
    SequenceDisposition disposition = SequenceDisposition::First;
    uint32_t gap = 0;
};

// RFC-1982 style serial arithmetic.  Exactly half the uint32 range is
// ambiguous and therefore rejected as stale; accepting it would permit an
// old command to refresh the lease after wrap.
inline SequenceCheck checkSequence(bool hasLast, uint32_t last,
                                   uint32_t candidate) {
    if (!hasLast) return {SequenceDisposition::First, 0};
    const uint32_t distance = candidate - last;
    if (distance == 0) return {SequenceDisposition::Duplicate, 0};
    if (distance >= 0x80000000u) return {SequenceDisposition::Stale, 0};
    if (distance == 1) return {SequenceDisposition::New, 0};
    return {SequenceDisposition::NewWithGap, distance - 1};
}

inline bool isNewSequence(SequenceDisposition disposition) {
    return disposition == SequenceDisposition::First ||
           disposition == SequenceDisposition::New ||
           disposition == SequenceDisposition::NewWithGap;
}

} // namespace makcm::stick
