#pragma once

#include <stdint.h>

namespace makcm {
namespace hid {

// Pure timing policy for synthetic mouse traffic. The firmware owns locking
// and supplies a monotonic microsecond clock; keeping the policy independent
// makes the experiment settings host-testable.
class MouseInjectionRateGate {
public:
    void configure(bool enabled, uint16_t rateHz) {
        enabled_ = enabled;
        rateHz_ = enabled ? rateHz : 0;
        intervalUs_ = rateHz_ == 0 ? 0 : 1000000ULL / rateHz_;
        resetTiming();
    }

    void resetTiming() {
        lastSubmitUs_ = 0;
        hasSubmitted_ = false;
    }

    bool allowed(uint64_t nowUs) const {
        return enabled_ &&
            (intervalUs_ == 0 || !hasSubmitted_ ||
             nowUs - lastSubmitUs_ >= intervalUs_);
    }

    void noteSubmit(uint64_t nowUs) {
        lastSubmitUs_ = nowUs;
        hasSubmitted_ = true;
    }

    bool enabled() const { return enabled_; }
    uint16_t rateHz() const { return rateHz_; }
    uint64_t intervalUs() const { return intervalUs_; }

private:
    bool enabled_ = true;
    uint16_t rateHz_ = 0;
    uint64_t intervalUs_ = 0;
    uint64_t lastSubmitUs_ = 0;
    bool hasSubmitted_ = false;
};

} // namespace hid
} // namespace makcm
