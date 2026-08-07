#pragma once

// Exact-profile controller report codecs.  This layer only decodes/encodes a
// supplied report buffer; it does not know about USB endpoints, FIFO policy,
// scheduling, or transport retries.  A profile must be explicitly configured
// before any mutation is possible.

#include <stdint.h>

#include <array>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>

namespace makcm::stick {

enum class CodecFieldEncoding : uint8_t {
    Int16LE = 0,
    UInt8 = 1,
};

enum class CodecError : uint8_t {
    None = 0,
    ProfileNotConfigured,
    ProfileMismatch,
    InvalidProfile,
    NullReport,
    WrongLength,
    WrongReportId,
    InvalidField,
    MutationGuard,
    OutputTooSmall,
};

struct StickVector {
    float x = 0.0f;
    float y = 0.0f;
};

struct StickQ15 {
    int16_t x = 0;
    int16_t y = 0;
};

using NormalizedStick = StickVector;

inline float clampNormalized(float value) {
    if (!std::isfinite(value)) return 0.0f;
    return value < -1.0f ? -1.0f : (value > 1.0f ? 1.0f : value);
}

inline StickVector clampNormalized(StickVector value) {
    value.x = clampNormalized(value.x);
    value.y = clampNormalized(value.y);
    return value;
}

inline int16_t normalizedToQ15(float value) {
    const float clamped = clampNormalized(value);
    // Q15 intentionally uses [-32767,32767].  The raw int16 negative endpoint
    // (-32768) remains available to an asymmetric int16 report range.
    const long rounded = std::lround(clamped * 32767.0f);
    return static_cast<int16_t>(rounded < -32767 ? -32767 :
                                (rounded > 32767 ? 32767 : rounded));
}

inline float q15ToNormalized(int16_t value) {
    const int32_t clamped = value < -32767 ? -32767 : value;
    return static_cast<float>(clamped) / 32767.0f;
}

struct ControllerProfileIdentity {
    uint16_t vid = 0;
    uint16_t pid = 0;
    uint16_t bcdDevice = 0;
    // The compact hashes are useful to firmware telemetry.  The full SHA-256
    // fields are also part of exact identity when populated by capture tools.
    uint32_t descriptorHash = 0;
    uint32_t hidReportHash = 0;
    std::array<uint8_t, 32> descriptorSha256{};
    std::array<uint8_t, 32> hidReportSha256{};
    uint8_t interfaceNumber = 0;
    uint8_t endpointAddress = 0;
    uint8_t reportId = 0;
    uint16_t reportLength = 0;
    uint16_t formatVersion = 1;
    uint16_t variant = 0;

    bool operator==(const ControllerProfileIdentity &other) const {
        return vid == other.vid && pid == other.pid &&
               bcdDevice == other.bcdDevice && descriptorHash == other.descriptorHash &&
               hidReportHash == other.hidReportHash && descriptorSha256 == other.descriptorSha256 &&
               hidReportSha256 == other.hidReportSha256 &&
               interfaceNumber == other.interfaceNumber && endpointAddress == other.endpointAddress &&
               reportId == other.reportId && reportLength == other.reportLength &&
               formatVersion == other.formatVersion && variant == other.variant;
    }
    bool operator!=(const ControllerProfileIdentity &other) const {
        return !(*this == other);
    }
};

struct StickAxisField {
    uint16_t offset = 0;
    CodecFieldEncoding encoding = CodecFieldEncoding::Int16LE;
    int32_t center = 0;
    int32_t minimum = -32768;
    int32_t maximum = 32767;
    int8_t sign = 1;

    uint8_t width() const {
        return encoding == CodecFieldEncoding::Int16LE ? 2 : 1;
    }
};

struct ControllerProfile {
    ControllerProfileIdentity identity{};
    StickAxisField x{};
    StickAxisField y{};

    bool validate() const {
        const auto validAxis = [&](const StickAxisField &axis) {
            if (axis.sign != 1 && axis.sign != -1) return false;
            if (axis.minimum >= axis.center || axis.center >= axis.maximum) return false;
            if (axis.encoding == CodecFieldEncoding::UInt8 &&
                (axis.minimum < 0 || axis.maximum > 255 || axis.center < 0 || axis.center > 255))
                return false;
            if (axis.encoding == CodecFieldEncoding::Int16LE &&
                (axis.minimum < -32768 || axis.maximum > 32767 || axis.center < -32768 || axis.center > 32767))
                return false;
            return static_cast<uint32_t>(axis.offset) + axis.width() <= identity.reportLength;
        };
        if (identity.reportLength == 0 || !validAxis(x) || !validAxis(y)) return false;
        if (x.offset == y.offset ||
            (x.offset < y.offset + y.width() && y.offset < x.offset + x.width()))
            return false;
        if (identity.reportId != 0 && identity.reportLength < 1) return false;
        return true;
    }
};

inline ControllerProfile makeXboxInt16LeProfile(
    const ControllerProfileIdentity &identity, StickAxisField x,
    StickAxisField y) {
    ControllerProfile profile;
    profile.identity = identity;
    profile.x = x;
    profile.y = y;
    profile.x.encoding = CodecFieldEncoding::Int16LE;
    profile.y.encoding = CodecFieldEncoding::Int16LE;
    return profile;
}

inline ControllerProfile makeDualSenseUsbUint8Profile(
    const ControllerProfileIdentity &identity, StickAxisField x,
    StickAxisField y) {
    ControllerProfile profile;
    profile.identity = identity;
    profile.x = x;
    profile.y = y;
    profile.x.encoding = CodecFieldEncoding::UInt8;
    profile.y.encoding = CodecFieldEncoding::UInt8;
    return profile;
}

inline StickAxisField int16LeAxis(uint16_t offset, int32_t center = 0,
                                  int32_t minimum = -32768,
                                  int32_t maximum = 32767, int8_t sign = 1) {
    StickAxisField axis;
    axis.offset = offset;
    axis.encoding = CodecFieldEncoding::Int16LE;
    axis.center = center;
    axis.minimum = minimum;
    axis.maximum = maximum;
    axis.sign = sign;
    return axis;
}

inline StickAxisField uint8Axis(uint16_t offset, int32_t center = 128,
                                int32_t minimum = 0, int32_t maximum = 255,
                                int8_t sign = 1) {
    StickAxisField axis;
    axis.offset = offset;
    axis.encoding = CodecFieldEncoding::UInt8;
    axis.center = center;
    axis.minimum = minimum;
    axis.maximum = maximum;
    axis.sign = sign;
    return axis;
}

struct MutationResult {
    bool copied = false;
    bool changed = false;
    size_t changedBytes = 0;
    CodecError error = CodecError::None;
};

class ControllerReportCodec {
public:
    ControllerReportCodec() = default;
    explicit ControllerReportCodec(const ControllerProfile &profile) {
        configure(profile);
    }

    bool configure(const ControllerProfile &profile) {
        if (!profile.validate()) {
            profile_ = {};
            configured_ = false;
            lastError_ = CodecError::InvalidProfile;
            return false;
        }
        profile_ = profile;
        configured_ = true;
        lastError_ = CodecError::None;
        return true;
    }

    // Exact profile matching is intentionally separate from configure(): the
    // caller can capture an identity, compare it against an allow-listed
    // profile, and only then arm this codec.
    bool configureExact(const ControllerProfile &profile,
                        const ControllerProfileIdentity &observed) {
        if (profile.identity != observed) {
            profile_ = {};
            configured_ = false;
            lastError_ = CodecError::ProfileMismatch;
            return false;
        }
        return configure(profile);
    }

    bool configured() const { return configured_; }
    const ControllerProfile &profile() const { return profile_; }
    CodecError lastError() const { return lastError_; }

    bool identityMatches(const ControllerProfileIdentity &observed) const {
        return configured_ && profile_.identity == observed;
    }

    bool decodePhysical(const uint8_t *report, size_t length,
                        StickVector &value, CodecError *error = nullptr) const {
        value = {};
        if (!validateReport(report, length, error)) return false;
        const int32_t rawX = readRaw(report + profile_.x.offset, profile_.x.encoding);
        const int32_t rawY = readRaw(report + profile_.y.offset, profile_.y.encoding);
        if (!validRaw(rawX, profile_.x) || !validRaw(rawY, profile_.y)) {
            setError(error, CodecError::InvalidField);
            return false;
        }
        value.x = decodeAxis(rawX, profile_.x);
        value.y = decodeAxis(rawY, profile_.y);
        setError(error, CodecError::None);
        return true;
    }

    bool decodePhysical(const uint8_t *report, size_t length, StickQ15 &value,
                        CodecError *error = nullptr) const {
        StickVector normalized;
        if (!decodePhysical(report, length, normalized, error)) {
            value = {};
            return false;
        }
        value.x = normalizedToQ15(normalized.x);
        value.y = normalizedToQ15(normalized.y);
        return true;
    }

    // Copy original -> compose on every path.  If this codec is not configured
    // or the report does not match its exact profile, callers still receive a
    // byte-for-byte raw-safe copy and `false`; no mutation is possible.
    bool composeCopy(const uint8_t *original, uint8_t *compose, size_t length,
                     StickVector value, MutationResult *result = nullptr) const {
        MutationResult local;
        MutationResult &outcome = result != nullptr ? *result : local;
        outcome = {};
        if (original == nullptr || compose == nullptr) {
            outcome.error = CodecError::NullReport;
            return false;
        }
        if (length != 0) {
            std::memcpy(compose, original, length);
            outcome.copied = true;
        }
        if (!configured_) {
            outcome.error = CodecError::ProfileNotConfigured;
            return false;
        }
        CodecError validation = CodecError::None;
        if (!validateReport(original, length, &validation)) {
            outcome.error = validation;
            return false;
        }
        value = clampNormalized(value);
        if (!writeRaw(compose + profile_.x.offset, profile_.x, value.x) ||
            !writeRaw(compose + profile_.y.offset, profile_.y, value.y)) {
            // Restore the original if a field write failed halfway through.
            std::memcpy(compose, original, length);
            outcome.error = CodecError::InvalidField;
            return false;
        }
        if (!validateUntouched(original, compose, length, &outcome)) {
            std::memcpy(compose, original, length);
            outcome.error = CodecError::MutationGuard;
            return false;
        }
        outcome.error = CodecError::None;
        return true;
    }

    bool composeCopyQ15(const uint8_t *original, uint8_t *compose, size_t length,
                        StickQ15 value, MutationResult *result = nullptr) const {
        return composeCopy(original, compose, length,
                           {q15ToNormalized(value.x), q15ToNormalized(value.y)},
                           result);
    }

    // In-place convenience wrapper for compose buffers that are already a
    // copy of the physical FIFO slot.
    bool encodeSynthetic(uint8_t *compose, size_t length, StickVector value,
                         MutationResult *result = nullptr) const {
        if (compose == nullptr) {
            if (result != nullptr) *result = {};
            if (result != nullptr) result->error = CodecError::NullReport;
            return false;
        }
        std::array<uint8_t, 1024> original{};
        if (length > original.size()) {
            if (result != nullptr) result->error = CodecError::OutputTooSmall;
            return false;
        }
        std::memcpy(original.data(), compose, length);
        return composeCopy(original.data(), compose, length, value, result);
    }

    bool validateUntouched(const uint8_t *original, const uint8_t *mutated,
                           size_t length, MutationResult *result = nullptr) const {
        if (original == nullptr || mutated == nullptr || length != profile_.identity.reportLength)
            return false;
        const auto allowed = [&](size_t index) {
            const size_t xEnd = profile_.x.offset + profile_.x.width();
            const size_t yEnd = profile_.y.offset + profile_.y.width();
            return (index >= profile_.x.offset && index < xEnd) ||
                   (index >= profile_.y.offset && index < yEnd);
        };
        MutationResult local;
        MutationResult &outcome = result != nullptr ? *result : local;
        outcome.changed = false;
        outcome.changedBytes = 0;
        for (size_t i = 0; i < length; ++i) {
            if (original[i] == mutated[i]) continue;
            if (!allowed(i)) return false;
            outcome.changed = true;
            ++outcome.changedBytes;
        }
        return true;
    }

private:
    static void setError(CodecError *error, CodecError value) {
        if (error != nullptr) *error = value;
    }

    bool validateReport(const uint8_t *report, size_t length,
                        CodecError *error) const {
        if (!configured_) {
            setError(error, CodecError::ProfileNotConfigured);
            return false;
        }
        if (report == nullptr) {
            setError(error, CodecError::NullReport);
            return false;
        }
        if (length != profile_.identity.reportLength) {
            setError(error, CodecError::WrongLength);
            return false;
        }
        if (profile_.identity.reportId != 0 && report[0] != profile_.identity.reportId) {
            setError(error, CodecError::WrongReportId);
            return false;
        }
        setError(error, CodecError::None);
        return true;
    }

    static int32_t readRaw(const uint8_t *data, CodecFieldEncoding encoding) {
        if (encoding == CodecFieldEncoding::UInt8) return data[0];
        const uint16_t encoded = static_cast<uint16_t>(data[0]) |
                                 (static_cast<uint16_t>(data[1]) << 8);
        return static_cast<int16_t>(encoded);
    }

    static bool validRaw(int32_t raw, const StickAxisField &axis) {
        return raw >= axis.minimum && raw <= axis.maximum;
    }

    static float decodeAxis(int32_t raw, const StickAxisField &axis) {
        const float denominator = raw >= axis.center
                                     ? static_cast<float>(axis.maximum - axis.center)
                                     : static_cast<float>(axis.center - axis.minimum);
        float normalized = denominator == 0.0f
                               ? 0.0f
                               : static_cast<float>(raw - axis.center) / denominator;
        normalized *= static_cast<float>(axis.sign);
        return clampNormalized(normalized);
    }

    static int32_t encodeRaw(float value, const StickAxisField &axis) {
        value = clampNormalized(value) * static_cast<float>(axis.sign);
        const float endpoint = value >= 0.0f
                                   ? static_cast<float>(axis.maximum - axis.center)
                                   : static_cast<float>(axis.center - axis.minimum);
        const long rounded = std::lround(static_cast<float>(axis.center) + value * endpoint);
        const long bounded = std::max<long>(axis.minimum, std::min<long>(axis.maximum, rounded));
        return static_cast<int32_t>(bounded);
    }

    static bool writeRaw(uint8_t *data, const StickAxisField &axis, float value) {
        if (data == nullptr) return false;
        const int32_t raw = encodeRaw(value, axis);
        if (!validRaw(raw, axis)) return false;
        if (axis.encoding == CodecFieldEncoding::UInt8) {
            data[0] = static_cast<uint8_t>(raw);
        } else {
            const uint16_t encoded = static_cast<uint16_t>(static_cast<int16_t>(raw));
            data[0] = static_cast<uint8_t>(encoded);
            data[1] = static_cast<uint8_t>(encoded >> 8);
        }
        return true;
    }

    ControllerProfile profile_{};
    bool configured_ = false;
    mutable CodecError lastError_ = CodecError::ProfileNotConfigured;
};

// Explicitly named wrappers make profile intent visible at call sites while
// retaining one implementation for all exact layouts.
class XboxStyleInt16LeCodec : public ControllerReportCodec {
public:
    XboxStyleInt16LeCodec() = default;
    explicit XboxStyleInt16LeCodec(const ControllerProfile &profile)
        : ControllerReportCodec(profile) {}
};

class DualSenseUsbUint8Codec : public ControllerReportCodec {
public:
    DualSenseUsbUint8Codec() = default;
    explicit DualSenseUsbUint8Codec(const ControllerProfile &profile)
        : ControllerReportCodec(profile) {}
};

} // namespace makcm::stick
