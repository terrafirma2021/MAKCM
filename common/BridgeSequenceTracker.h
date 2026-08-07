#pragma once

#include <atomic>
#include <stdint.h>

namespace makcm::bridge {

// Tracks the independent sequence stream emitted by the right-side endpoint
// TX task.  Profile/control frames are deliberately not fed to this class:
// those frames may be retried with sequence zero, while EndpointIn* frames
// use one monotonically increasing uint16 stream per running host task.
//
// The writer is the bridge RX task.  The counters are atomic because status
// requests can read them from another task/core.  A session change starts a
// new stream and therefore is not reported as a gap.
class InputSequenceTracker final {
public:
    void reset() {
        initialized_.store(false, std::memory_order_release);
        session_.store(0, std::memory_order_release);
        expected_.store(0, std::memory_order_release);
        frames_.store(0, std::memory_order_relaxed);
        gap_frames_.store(0, std::memory_order_relaxed);
        gap_events_.store(0, std::memory_order_relaxed);
        duplicates_.store(0, std::memory_order_relaxed);
        out_of_order_.store(0, std::memory_order_relaxed);
        session_changes_.store(0, std::memory_order_relaxed);
    }

    void observe(uint32_t stream_session, uint16_t sequence) {
        frames_.fetch_add(1, std::memory_order_relaxed);

        if (!initialized_.load(std::memory_order_acquire) ||
            session_.load(std::memory_order_acquire) != stream_session) {
            if (initialized_.load(std::memory_order_acquire)) {
                session_changes_.fetch_add(1, std::memory_order_relaxed);
            }
            session_.store(stream_session, std::memory_order_release);
            expected_.store(static_cast<uint16_t>(sequence + 1),
                            std::memory_order_release);
            initialized_.store(true, std::memory_order_release);
            return;
        }

        const uint16_t expected = expected_.load(std::memory_order_acquire);
        const uint16_t delta = static_cast<uint16_t>(sequence - expected);
        if (delta == 0) {
            expected_.store(static_cast<uint16_t>(sequence + 1),
                            std::memory_order_release);
            return;
        }

        // Modular subtraction lets a normal 0xffff -> 0 wrap pass.  Values
        // in the forward half of the ring indicate missing frames; values in
        // the backward half are an old/duplicate frame.
        if (delta < 0x8000u) {
            gap_frames_.fetch_add(delta, std::memory_order_relaxed);
            gap_events_.fetch_add(1, std::memory_order_relaxed);
            expected_.store(static_cast<uint16_t>(sequence + 1),
                            std::memory_order_release);
            return;
        }

        out_of_order_.fetch_add(1, std::memory_order_relaxed);
        if (sequence == static_cast<uint16_t>(expected - 1)) {
            duplicates_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    uint32_t frames() const { return frames_.load(std::memory_order_relaxed); }
    uint32_t gapFrames() const {
        return gap_frames_.load(std::memory_order_relaxed);
    }
    uint32_t gapEvents() const {
        return gap_events_.load(std::memory_order_relaxed);
    }
    uint32_t duplicates() const {
        return duplicates_.load(std::memory_order_relaxed);
    }
    uint32_t outOfOrder() const {
        return out_of_order_.load(std::memory_order_relaxed);
    }
    uint32_t sessionChanges() const {
        return session_changes_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<bool> initialized_{false};
    std::atomic<uint32_t> session_{0};
    std::atomic<uint16_t> expected_{0};
    std::atomic<uint32_t> frames_{0};
    std::atomic<uint32_t> gap_frames_{0};
    std::atomic<uint32_t> gap_events_{0};
    std::atomic<uint32_t> duplicates_{0};
    std::atomic<uint32_t> out_of_order_{0};
    std::atomic<uint32_t> session_changes_{0};
};

} // namespace makcm::bridge
