#pragma once

#include <stdint.h>

#include "HidMouseReport.h"
#include "profile_store.h"

namespace makcm::native {

// Optional input interpretation is deliberately isolated from the USB
// transport.  A profile that is not explicitly recognized stays on the raw
// endpoint path and cannot be affected by these helpers.
class InputPolicy final {
public:
    void configure(const ProfileStore *profile) {
        mode_ = Mode::None;
        controller_endpoint_ = 0;
        if (profile == nullptr) return;
        const auto &identity = profile->profile();
        if (identity.vid == 0x054C && identity.pid == 0x0DF2) {
            mode_ = Mode::DseEdge;
        } else if (identity.vid == 0x4131 && identity.pid == 0x3519) {
            mode_ = Mode::KnownController;
        }
        const EndpointInfo *endpoint = profile->xinputEndpoint();
        if (endpoint != nullptr) controller_endpoint_ = endpoint->address;
    }

    bool hasControllerButtonPolicy() const { return mode_ != Mode::None; }
    bool isControllerEndpoint(uint8_t address) const {
        return hasControllerButtonPolicy() && controller_endpoint_ != 0 &&
               controller_endpoint_ == address;
    }

    bool decodeControllerButtons(const uint8_t *report, uint16_t length,
                                 uint8_t oldMask, uint8_t &newMask) const {
        if (report == nullptr) return false;
        bool fire = false;
        bool ads = false;
        const bool oldFire = (oldMask & 0x01) != 0;
        const bool oldAds = (oldMask & 0x02) != 0;
        if (mode_ == Mode::DseEdge && length == 64 && report[0] == 0x01) {
            fire = oldFire ? report[6] > 12 : report[6] >= 24;
            ads = oldAds ? report[5] > 12 : report[5] >= 24;
        } else if (mode_ == Mode::KnownController && length >= 6 &&
                   report[0] == 0x00 && report[1] >= 6) {
            fire = oldFire ? report[5] > 12 : report[5] >= 24;
            ads = oldAds ? report[4] > 12 : report[4] >= 24;
        } else {
            return false;
        }
        newMask = static_cast<uint8_t>((fire ? 0x01 : 0) |
                                       (ads ? 0x02 : 0));
        return true;
    }

    bool fallbackMouseLayout(makcm::hid::MouseReportLayout &layout) const {
        if (mode_ != Mode::KnownController) return false;
        layout = {};
        layout.valid = true;
        layout.hasReportId = true;
        layout.reportId = 0x04;
        layout.reportLength = 7;
        for (uint8_t i = 0; i < 5; ++i) {
            layout.buttons[i].present = true;
            layout.buttons[i].bitOffset = i;
            layout.buttons[i].bitSize = 1;
            layout.buttons[i].logicalMinimum = 0;
            layout.buttons[i].logicalMaximum = 1;
        }
        layout.x.present = true;
        layout.x.bitOffset = 8;
        layout.x.bitSize = 16;
        layout.x.logicalMinimum = -32767;
        layout.x.logicalMaximum = 32767;
        layout.y.present = true;
        layout.y.bitOffset = 24;
        layout.y.bitSize = 16;
        layout.y.logicalMinimum = -32767;
        layout.y.logicalMaximum = 32767;
        layout.wheel.present = true;
        layout.wheel.bitOffset = 40;
        layout.wheel.bitSize = 8;
        layout.wheel.logicalMinimum = -127;
        layout.wheel.logicalMaximum = 127;
        return true;
    }

private:
    enum class Mode : uint8_t { None, DseEdge, KnownController };
    Mode mode_ = Mode::None;
    uint8_t controller_endpoint_ = 0;
};

} // namespace makcm::native
