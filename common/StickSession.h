#pragma once

#include "StickProtocol.h"

#include <stdint.h>

#include <algorithm>

namespace makcm::stick {

enum class StickState : uint8_t {
    RawSafe = 0,
    StickArming = 1,
    StickIdle = 2,
    StickActive = 3,
    PhysicalOverride = 4,
    Expired = 5,
    Fault = 6,
    Hotplug = 7,
};

enum class SessionError : uint8_t {
    None = 0,
    Accepted,
    UnboundPhysicalSession,
    WrongPhysicalSession,
    UnboundInjectionSession,
    WrongInjectionSession,
    SourceConflict,
    InvalidTtl,
    DuplicateSequence,
    StaleSequence,
    Faulted,
};

struct SessionCounters {
    uint32_t commandsReceived = 0;
    uint32_t commandsAccepted = 0;
    uint32_t commandsRejected = 0;
    uint32_t wrongPhysicalSession = 0;
    uint32_t wrongInjectionSession = 0;
    uint32_t sourceConflict = 0;
    uint32_t duplicateSequence = 0;
    uint32_t staleSequence = 0;
    uint32_t sequenceGap = 0;
    uint32_t ttlExpiry = 0;
    uint32_t leaseExpiry = 0;
};

struct AcceptResult {
    bool accepted = false;
    SessionError error = SessionError::None;
    SequenceCheck sequence{};
};

struct StickSample {
    int32_t x = 0;
    int32_t y = 0;
    bool centered = true;
    bool expired = false;
    uint16_t ageMs = 0;
};

// Session and source validation intentionally live together.  A source is
// bound to one injection session; changing state/rate/move-compat requires a
// new injection session (and therefore a fresh sequence/TTL history).
class StickSession {
public:
    void reset() {
        physicalSession_ = 0;
        injectionSession_ = 0;
        source_ = StickSource::None;
        state_ = StickState::RawSafe;
        hasSequence_ = false;
        lastSequence_ = 0;
        valueX_ = 0;
        valueY_ = 0;
        acceptedAtMs_ = 0;
        ttlMs_ = kDefaultTtlMs;
        expiredCounted_ = false;
        counters_ = {};
    }

    // A physical-session change is a hard security boundary.  No prior
    // injection command survives a hot-plug/re-enumeration transition.
    bool bindPhysical(uint32_t physicalSession, uint64_t nowMs = 0) {
        if (physicalSession == 0) return false;
        if (physicalSession_ == physicalSession && state_ != StickState::Hotplug)
            return true;
        physicalSession_ = physicalSession;
        injectionSession_ = 0;
        source_ = StickSource::None;
        hasSequence_ = false;
        valueX_ = valueY_ = 0;
        acceptedAtMs_ = nowMs;
        ttlMs_ = kDefaultTtlMs;
        expiredCounted_ = false;
        state_ = StickState::RawSafe;
        return true;
    }

    void hotplug(uint64_t nowMs = 0) {
        (void)nowMs;
        physicalSession_ = 0;
        injectionSession_ = 0;
        source_ = StickSource::None;
        hasSequence_ = false;
        valueX_ = valueY_ = 0;
        acceptedAtMs_ = 0;
        ttlMs_ = kDefaultTtlMs;
        expiredCounted_ = false;
        state_ = StickState::Hotplug;
    }

    // Bind a source and injection session after the caller has completed its
    // mode-transition barrier.  A new injection ID deliberately clears all
    // sequence and TTL state even when the physical session is unchanged.
    bool beginInjection(uint32_t physicalSession, uint32_t injectionSession,
                        StickSource source, uint64_t nowMs = 0) {
        if (physicalSession == 0 || injectionSession == 0 || source == StickSource::None)
            return false;
        if (physicalSession_ != physicalSession) return false;
        injectionSession_ = injectionSession;
        source_ = source;
        hasSequence_ = false;
        lastSequence_ = 0;
        valueX_ = valueY_ = 0;
        acceptedAtMs_ = nowMs;
        ttlMs_ = kDefaultTtlMs;
        expiredCounted_ = false;
        state_ = StickState::StickArming;
        return true;
    }

    // Convenience overload for callers that have already bound the physical
    // session with bindPhysical().
    bool beginInjection(uint32_t injectionSession, StickSource source,
                        uint64_t nowMs = 0) {
        return beginInjection(physicalSession_, injectionSession, source, nowMs);
    }

    void disarm() {
        injectionSession_ = 0;
        source_ = StickSource::None;
        hasSequence_ = false;
        valueX_ = valueY_ = 0;
        acceptedAtMs_ = 0;
        ttlMs_ = kDefaultTtlMs;
        expiredCounted_ = false;
        if (physicalSession_ != 0) state_ = StickState::RawSafe;
        else state_ = StickState::Hotplug;
    }

    AcceptResult accept(const StickCommand &command, uint32_t physicalSession,
                        uint64_t nowMs) {
        ++counters_.commandsReceived;
        AcceptResult result;
        if (state_ == StickState::Fault) {
            ++counters_.commandsRejected;
            result.error = SessionError::Faulted;
            return result;
        }
        if (physicalSession == 0 || physicalSession_ == 0) {
            ++counters_.wrongPhysicalSession;
            ++counters_.commandsRejected;
            state_ = StickState::Fault;
            result.error = SessionError::UnboundPhysicalSession;
            return result;
        }
        if (physicalSession != physicalSession_ ||
            (command.physicalSession != 0 && command.physicalSession != physicalSession_)) {
            ++counters_.wrongPhysicalSession;
            ++counters_.commandsRejected;
            state_ = StickState::Fault;
            result.error = SessionError::WrongPhysicalSession;
            return result;
        }
        if (injectionSession_ == 0) {
            ++counters_.wrongInjectionSession;
            ++counters_.commandsRejected;
            result.error = SessionError::UnboundInjectionSession;
            return result;
        }
        if (command.injectionSession != injectionSession_) {
            ++counters_.wrongInjectionSession;
            ++counters_.commandsRejected;
            result.error = SessionError::WrongInjectionSession;
            return result;
        }
        if (command.source == StickSource::None || command.source != source_ ||
            sourceForKind(command.kind) != command.source) {
            ++counters_.sourceConflict;
            ++counters_.commandsRejected;
            result.error = SessionError::SourceConflict;
            return result;
        }
        if (command.ttlMs < kMinTtlMs || command.ttlMs > kMaxTtlMs) {
            ++counters_.commandsRejected;
            result.error = SessionError::InvalidTtl;
            return result;
        }

        result.sequence = checkSequence(hasSequence_, lastSequence_, command.sequence);
        if (result.sequence.disposition == SequenceDisposition::Duplicate) {
            ++counters_.duplicateSequence;
            ++counters_.commandsRejected;
            result.error = SessionError::DuplicateSequence;
            // A duplicate must never refresh acceptedAtMs_ or TTL.
            return result;
        }
        if (result.sequence.disposition == SequenceDisposition::Stale) {
            ++counters_.staleSequence;
            ++counters_.commandsRejected;
            result.error = SessionError::StaleSequence;
            return result;
        }
        if (result.sequence.disposition == SequenceDisposition::NewWithGap)
            counters_.sequenceGap += result.sequence.gap;

        hasSequence_ = true;
        lastSequence_ = command.sequence;
        valueX_ = command.x;
        valueY_ = command.y;
        acceptedAtMs_ = nowMs;
        ttlMs_ = command.ttlMs;
        expiredCounted_ = false;
        state_ = (command.x == 0 && command.y == 0)
                     ? StickState::StickIdle
                     : StickState::StickActive;
        ++counters_.commandsAccepted;
        result.accepted = true;
        result.error = SessionError::Accepted;
        return result;
    }

    AcceptResult accept(const StickCommand &command, uint64_t nowMs) {
        const uint32_t physical = command.physicalSession != 0
                                      ? command.physicalSession
                                      : physicalSession_;
        return accept(command, physical, nowMs);
    }

    // Returns the latest accepted command while it is within TTL.  Once the
    // deadline passes this function always returns a logical center and never
    // resurrects the previous command.
    StickSample sample(uint64_t nowMs) {
        StickSample result;
        // Any fault or unbound/raw-safe state is immediately logical center;
        // in particular a wrong physical-session frame must not leave the
        // previous synthetic value visible until its old TTL happens to end.
        if (state_ == StickState::Fault || state_ == StickState::Hotplug ||
            state_ == StickState::RawSafe) {
            result.centered = true;
            return result;
        }
        if (!hasSequence_ || injectionSession_ == 0 || source_ == StickSource::None) {
            result.centered = true;
            result.ageMs = 0;
            return result;
        }
        const uint64_t elapsed = nowMs >= acceptedAtMs_ ? nowMs - acceptedAtMs_ : 0;
        result.ageMs = static_cast<uint16_t>(std::min<uint64_t>(elapsed, 0xFFFFu));
        if (elapsed >= ttlMs_) {
            result.expired = true;
            result.centered = true;
            result.x = result.y = 0;
            state_ = StickState::Expired;
            if (!expiredCounted_) {
                ++counters_.ttlExpiry;
                if (source_ == StickSource::MoveCompat) ++counters_.leaseExpiry;
                expiredCounted_ = true;
            }
            return result;
        }
        result.x = valueX_;
        result.y = valueY_;
        result.centered = valueX_ == 0 && valueY_ == 0;
        return result;
    }

    // Read the current logical command without advancing TTL state.  This is
    // intentionally separate from sample(): status/telemetry callers must not
    // be able to turn an otherwise active session into Expired merely by
    // querying it.  compose() and the watchdog continue to use sample() when
    // they own the state transition.
    StickSample peek(uint64_t nowMs) const {
        StickSample result;
        // Match sample()'s fail-closed view, but do not mutate state_ or the
        // expiry counters while producing the snapshot.
        if (state_ == StickState::Fault || state_ == StickState::Hotplug ||
            state_ == StickState::RawSafe) {
            result.centered = true;
            return result;
        }
        if (!hasSequence_ || injectionSession_ == 0 ||
            source_ == StickSource::None) {
            result.centered = true;
            result.ageMs = 0;
            return result;
        }
        const uint64_t elapsed = nowMs >= acceptedAtMs_ ? nowMs - acceptedAtMs_ : 0;
        result.ageMs = static_cast<uint16_t>(std::min<uint64_t>(elapsed, 0xFFFFu));
        if (elapsed >= ttlMs_) {
            result.expired = true;
            result.centered = true;
            result.x = result.y = 0;
            return result;
        }
        result.x = valueX_;
        result.y = valueY_;
        result.centered = valueX_ == 0 && valueY_ == 0;
        return result;
    }

    bool expired(uint64_t nowMs) { return sample(nowMs).expired; }

    uint32_t physicalSession() const { return physicalSession_; }
    uint32_t injectionSession() const { return injectionSession_; }
    StickSource source() const { return source_; }
    StickState state() const { return state_; }
    bool hasSequence() const { return hasSequence_; }
    uint32_t lastSequence() const { return lastSequence_; }
    uint16_t ttlMs() const { return ttlMs_; }
    uint64_t acceptedAtMs() const { return acceptedAtMs_; }
    const SessionCounters &counters() const { return counters_; }
    SessionCounters &counters() { return counters_; }

    void setState(StickState state) { state_ = state; }

private:
    uint32_t physicalSession_ = 0;
    uint32_t injectionSession_ = 0;
    StickSource source_ = StickSource::None;
    StickState state_ = StickState::RawSafe;
    bool hasSequence_ = false;
    uint32_t lastSequence_ = 0;
    int32_t valueX_ = 0;
    int32_t valueY_ = 0;
    uint64_t acceptedAtMs_ = 0;
    uint16_t ttlMs_ = kDefaultTtlMs;
    bool expiredCounted_ = false;
    SessionCounters counters_{};
};

} // namespace makcm::stick
