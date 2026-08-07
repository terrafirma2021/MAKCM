#pragma once

#include "ControllerCodec.h"

#include <stdint.h>

#include <cmath>

namespace makcm::stick {

struct PhysicalPriorityConfig {
    float overrideOn = 0.10f;
    float overrideOff = 0.06f;
    uint32_t stableMs = 20;
};

inline float stickMagnitude(StickVector value) {
    value = clampNormalized(value);
    return std::sqrt(value.x * value.x + value.y * value.y);
}

inline StickVector circleClamp(StickVector value) {
    value = clampNormalized(value);
    const float magnitude = std::sqrt(value.x * value.x + value.y * value.y);
    if (magnitude <= 1.0f || magnitude == 0.0f) return value;
    value.x /= magnitude;
    value.y /= magnitude;
    return value;
}

inline float axisDeadzone(float value, float deadzone) {
    value = clampNormalized(value);
    deadzone = std::max(0.0f, std::min(0.99f, deadzone));
    const float magnitude = std::fabs(value);
    if (magnitude <= deadzone) return 0.0f;
    const float scaled = (magnitude - deadzone) / (1.0f - deadzone);
    return value < 0.0f ? -scaled : scaled;
}

inline StickVector radialDeadzone(StickVector value, float deadzone) {
    value = circleClamp(value);
    deadzone = std::max(0.0f, std::min(0.99f, deadzone));
    const float magnitude = stickMagnitude(value);
    if (magnitude <= deadzone || magnitude == 0.0f) return {};
    const float scaled = (magnitude - deadzone) / (1.0f - deadzone);
    value.x = value.x / magnitude * scaled;
    value.y = value.y / magnitude * scaled;
    return circleClamp(value);
}

// First-version physical merge policy.  A latched physical override always
// wins and synthetic state is held at logical center.  After the latch drops,
// a *fresh* injection sequence is required before synthetic output can resume;
// this prevents a pre-override command from silently reappearing.
class PhysicalPriorityOverride {
public:
    explicit PhysicalPriorityOverride(PhysicalPriorityConfig config = {})
        : config_(config) {
        if (config_.overrideOn < 0.0f) config_.overrideOn = 0.0f;
        if (config_.overrideOff < 0.0f) config_.overrideOff = 0.0f;
        if (config_.overrideOff > config_.overrideOn)
            config_.overrideOff = config_.overrideOn;
    }

    void reset() {
        latched_ = false;
        awaitingFresh_ = false;
        belowSinceMs_ = 0;
        haveBelowSince_ = false;
        haveSequence_ = false;
        lastSequence_ = 0;
    }

    bool observe(StickVector physical, uint64_t nowMs) {
        const float magnitude = stickMagnitude(physical);
        if (!latched_) {
            if (magnitude >= config_.overrideOn) {
                latched_ = true;
                awaitingFresh_ = false;
                haveBelowSince_ = false;
            }
            return latched_;
        }
        if (magnitude >= config_.overrideOff) {
            haveBelowSince_ = false;
            return true;
        }
        if (!haveBelowSince_) {
            haveBelowSince_ = true;
            belowSinceMs_ = nowMs;
            return true;
        }
        const uint64_t elapsed = nowMs >= belowSinceMs_ ? nowMs - belowSinceMs_ : 0;
        if (elapsed >= config_.stableMs) {
            latched_ = false;
            awaitingFresh_ = true;
            haveBelowSince_ = false;
        }
        return latched_;
    }

    // Sequence-aware overload: observing a fresh sequence is enough to clear
    // the post-override barrier, but only if the physical latch is already
    // released. Duplicate/stale sequence numbers do not restore output.
    bool observe(StickVector physical, uint32_t commandSequence,
                 uint64_t nowMs) {
        if (!haveSequence_ || commandSequence != lastSequence_) {
            haveSequence_ = true;
            lastSequence_ = commandSequence;
            if (!latched_ && awaitingFresh_) awaitingFresh_ = false;
        }
        return observe(physical, nowMs);
    }

    void notifyFreshSequence(uint32_t sequence) {
        if (!haveSequence_ || sequence != lastSequence_) {
            haveSequence_ = true;
            lastSequence_ = sequence;
            if (!latched_) awaitingFresh_ = false;
        }
    }

    bool latched() const { return latched_; }
    bool awaitingFresh() const { return awaitingFresh_; }
    bool syntheticAllowed() const { return !latched_ && !awaitingFresh_; }
    const PhysicalPriorityConfig &config() const { return config_; }

    StickVector merge(StickVector physical, StickVector synthetic) const {
        physical = clampNormalized(physical);
        synthetic = clampNormalized(synthetic);
        if (latched_) return physical;
        if (awaitingFresh_) return {};
        // Neutral-only policy: physical input remains authoritative whenever
        // it is non-neutral; synthetic output is used only around center.
        if (stickMagnitude(physical) >= config_.overrideOff) return physical;
        return synthetic;
    }

private:
    PhysicalPriorityConfig config_{};
    bool latched_ = false;
    bool awaitingFresh_ = false;
    bool haveBelowSince_ = false;
    uint64_t belowSinceMs_ = 0;
    bool haveSequence_ = false;
    uint32_t lastSequence_ = 0;
};

inline StickVector physicalPriorityMerge(StickVector physical,
                                          StickVector synthetic,
                                          const PhysicalPriorityOverride &state) {
    return state.merge(physical, synthetic);
}

} // namespace makcm::stick
