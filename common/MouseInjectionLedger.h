#pragma once

#include "HidMouseReport.h"
#include <stdint.h>

namespace makcm {
namespace hid {

struct MouseInjectionSnapshot {
    int64_t x = 0;
    int64_t y = 0;
    int64_t wheel = 0;
    int64_t pan = 0;
    uint8_t buttons = 0;
    uint32_t buttonGeneration = 0;
    uint32_t buttonSentGeneration = 0;
};

// A relative-integer delivery ledger.  Values stay pending until the matching
// USB IN transfer completes successfully.  Taking a snapshot never reserves or
// removes data, so a submit/completion failure needs no rollback.
class MouseInjectionLedger {
public:
    void reset() {
        pendingX_ = 0;
        pendingY_ = 0;
        pendingWheel_ = 0;
        pendingPan_ = 0;
        buttons_ = 0;
        buttonGeneration_ = 0;
        buttonSentGeneration_ = 0;
        reservation_ = {};
        reservationButtonGeneration_ = 0;
        reservationActive_ = false;
    }

    bool add(int64_t x, int64_t y, int64_t wheel, int64_t pan,
             bool wheelSupported, bool panSupported) {
        int64_t nextX = pendingX_;
        int64_t nextY = pendingY_;
        int64_t nextWheel = pendingWheel_;
        int64_t nextPan = pendingPan_;
        if (!checkedAdd(nextX, x) || !checkedAdd(nextY, y) ||
            (wheelSupported ? !checkedAdd(nextWheel, wheel) : wheel != 0) ||
            (panSupported ? !checkedAdd(nextPan, pan) : pan != 0)) {
            return false;
        }
        // A successful USB completion must be able to retire the in-flight
        // contribution without signed overflow, even if new commands arrive
        // with the opposite sign while it is active. A failure leaves pending
        // unchanged and therefore needs no rollback.
        if (reservationActive_) {
            int64_t successX = nextX;
            int64_t successY = nextY;
            int64_t successWheel = nextWheel;
            int64_t successPan = nextPan;
            if (!checkedSubtract(successX, reservation_.x) ||
                !checkedSubtract(successY, reservation_.y) ||
                !checkedSubtract(successWheel, reservation_.wheel) ||
                !checkedSubtract(successPan, reservation_.pan)) {
                return false;
            }
        }
        pendingX_ = nextX;
        pendingY_ = nextY;
        pendingWheel_ = nextWheel;
        pendingPan_ = nextPan;
        return true;
    }

    bool setButton(uint8_t buttonMask, bool pressed) {
        const uint8_t next = pressed
            ? static_cast<uint8_t>(buttons_ | buttonMask)
            : static_cast<uint8_t>(buttons_ & ~buttonMask);
        if (next == buttons_) return false;
        buttons_ = next;
        ++buttonGeneration_;
        return true;
    }

    MouseInjectionSnapshot snapshot() const {
        MouseInjectionSnapshot value;
        value.x = pendingX_;
        value.y = pendingY_;
        value.wheel = pendingWheel_;
        value.pan = pendingPan_;
        value.buttons = buttons_;
        value.buttonGeneration = buttonGeneration_;
        value.buttonSentGeneration = buttonSentGeneration_;
        return value;
    }

    bool pending() const {
        return pendingX_ != 0 || pendingY_ != 0 || pendingWheel_ != 0 ||
            pendingPan_ != 0 ||
            buttonGeneration_ != buttonSentGeneration_;
    }

    // A bus reset forgets the host-side button state but does not end the
    // physical-device session.  Keep relative deltas and the latch, and force
    // a held button state to be emitted again after enumeration.
    void markHostReset() {
        settle(false);
        if (buttons_ != 0) ++buttonGeneration_;
    }

    // Register the contribution before handing its report to TinyUSB. Pending
    // remains unchanged until a full successful completion, so submission and
    // transfer failures require no arithmetic rollback.
    bool reserve(const MouseMergeDelta &delivered,
                 uint32_t deliveredButtonGeneration) {
        if (reservationActive_) return false;
        int64_t successX = pendingX_;
        int64_t successY = pendingY_;
        int64_t successWheel = pendingWheel_;
        int64_t successPan = pendingPan_;
        if (!checkedSubtract(successX, delivered.x) ||
            !checkedSubtract(successY, delivered.y) ||
            !checkedSubtract(successWheel, delivered.wheel) ||
            !checkedSubtract(successPan, delivered.pan)) {
            return false;
        }
        reservation_ = delivered;
        reservationButtonGeneration_ = deliveredButtonGeneration;
        reservationActive_ = true;
        return true;
    }

    // Success retires the reservation; failure only clears it. New commands may
    // have arrived since reserve(), but add() already checked both outcomes.
    bool settle(bool success) {
        if (!reservationActive_) return false;
        if (success) {
            int64_t nextX = pendingX_;
            int64_t nextY = pendingY_;
            int64_t nextWheel = pendingWheel_;
            int64_t nextPan = pendingPan_;
            if (!checkedSubtract(nextX, reservation_.x) ||
                !checkedSubtract(nextY, reservation_.y) ||
                !checkedSubtract(nextWheel, reservation_.wheel) ||
                !checkedSubtract(nextPan, reservation_.pan)) {
                return false;
            }
            pendingX_ = nextX;
            pendingY_ = nextY;
            pendingWheel_ = nextWheel;
            pendingPan_ = nextPan;
            buttonSentGeneration_ = reservationButtonGeneration_;
        }
        reservation_ = {};
        reservationButtonGeneration_ = 0;
        reservationActive_ = false;
        return true;
    }

    bool reservationActive() const { return reservationActive_; }

private:
    static bool checkedAdd(int64_t &target, int64_t value) {
        if (value > 0 && target > INT64_MAX - value) return false;
        if (value < 0 && target < INT64_MIN - value) return false;
        target += value;
        return true;
    }

    static bool checkedSubtract(int64_t &target, int64_t value) {
        if (value > 0 && target < INT64_MIN + value) return false;
        if (value < 0 && target > INT64_MAX + value) return false;
        target -= value;
        return true;
    }

    int64_t pendingX_ = 0;
    int64_t pendingY_ = 0;
    int64_t pendingWheel_ = 0;
    int64_t pendingPan_ = 0;
    uint8_t buttons_ = 0;
    uint32_t buttonGeneration_ = 0;
    uint32_t buttonSentGeneration_ = 0;
    MouseMergeDelta reservation_ = {};
    uint32_t reservationButtonGeneration_ = 0;
    bool reservationActive_ = false;
};

} // namespace hid
} // namespace makcm
