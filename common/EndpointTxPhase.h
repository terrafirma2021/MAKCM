#pragma once

#include <stdint.h>

namespace makcm {
namespace usb {

enum class EndpointPhase : uint8_t {
    Closed = 0,
    Idle,
    Starting,
    InFlight,
    Completing,
};

enum class EndpointCompletion : uint8_t {
    Stale = 0,
    Delivered,
    Retry,
};

// Pure state gate shared by the firmware and host tests. Buffer ownership and
// locking remain in the caller; every method is invoked inside its short
// endpoint critical section.
class EndpointTxPhase {
public:
    void reset() {
        phase_ = EndpointPhase::Closed;
        epoch_ = 0;
        submittedLength_ = 0;
    }

    void open(uint32_t epoch) {
        phase_ = EndpointPhase::Idle;
        epoch_ = epoch;
        submittedLength_ = 0;
    }

    bool begin(uint32_t epoch) {
        if (phase_ != EndpointPhase::Idle || epoch_ != epoch) return false;
        phase_ = EndpointPhase::Starting;
        return true;
    }

    bool cancelStarting(uint32_t epoch) {
        if (phase_ != EndpointPhase::Starting || epoch_ != epoch) return false;
        phase_ = EndpointPhase::Idle;
        return true;
    }

    bool arm(uint32_t epoch, uint16_t submittedLength) {
        if (phase_ != EndpointPhase::Starting || epoch_ != epoch ||
            submittedLength == 0) {
            return false;
        }
        submittedLength_ = submittedLength;
        phase_ = EndpointPhase::InFlight;
        return true;
    }

    // Safe even when an early completion ran before usbd_edpt_xfer() returned:
    // it only changes the exact still-in-flight ticket.
    bool submitFailed(uint32_t epoch) {
        if (phase_ != EndpointPhase::InFlight || epoch_ != epoch) return false;
        submittedLength_ = 0;
        phase_ = EndpointPhase::Idle;
        return true;
    }

    EndpointCompletion beginCompletion(uint32_t currentEpoch,
                                       bool transferSuccess,
                                       uint32_t transferred,
                                       bool requireExactLength = true) {
        if (phase_ != EndpointPhase::InFlight || epoch_ != currentEpoch) {
            return EndpointCompletion::Stale;
        }
        phase_ = EndpointPhase::Completing;
        const bool lengthOk = requireExactLength
            ? transferred == submittedLength_
            : transferred <= submittedLength_;
        return transferSuccess && lengthOk
            ? EndpointCompletion::Delivered
            : EndpointCompletion::Retry;
    }

    bool finishCompletion(uint32_t epoch) {
        if (phase_ != EndpointPhase::Completing || epoch_ != epoch) return false;
        submittedLength_ = 0;
        phase_ = EndpointPhase::Idle;
        return true;
    }

    EndpointPhase phase() const { return phase_; }
    uint32_t epoch() const { return epoch_; }
    uint16_t submittedLength() const { return submittedLength_; }

private:
    EndpointPhase phase_ = EndpointPhase::Closed;
    uint32_t epoch_ = 0;
    uint16_t submittedLength_ = 0;
};

} // namespace usb
} // namespace makcm
