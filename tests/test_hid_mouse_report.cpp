#include "HidMouseReport.h"

#include <assert.h>
#include <stdint.h>

int main() {
    const uint8_t xboxMouseDescriptor[] = {
        0x05,0x01,0x09,0x02,0xA1,0x01,0x85,0x04,0x05,0x01,0x09,0x01,
        0xA1,0x00,0x05,0x09,0x19,0x01,0x29,0x05,0x15,0x00,0x25,0x01,
        0x75,0x01,0x95,0x05,0x81,0x02,0x95,0x03,0x81,0x01,0x05,0x01,
        0x09,0x30,0x09,0x31,0x16,0x01,0x80,0x26,0xFF,0x7F,0x75,0x10,
        0x95,0x02,0x81,0x06,0x09,0x38,0x15,0x81,0x25,0x7F,0x75,0x08,
        0x95,0x01,0x81,0x06,0xC0,0xC0,
    };
    makcm::hid::MouseReportLayout layout;
    assert(makcm::hid::parseMouseReportDescriptor(
        xboxMouseDescriptor, sizeof(xboxMouseDescriptor), layout));
    assert(layout.valid);
    assert(layout.hasReportId && layout.reportId == 0x04);
    assert(layout.reportLength == 7);
    assert(layout.x.bitOffset == 8 && layout.x.bitSize == 16);
    assert(layout.y.bitOffset == 24 && layout.y.bitSize == 16);
    assert(layout.wheel.bitOffset == 40 && layout.wheel.bitSize == 8);
    assert(layout.x.logicalMinimum == -32767);
    assert(layout.x.logicalMaximum == 32767);

    const uint8_t physical[] = {0x04, 0x02, 0, 0, 0, 0, 0};
    uint8_t report[64] = {};
    assert(makcm::hid::composeMouseReport(
        layout, physical, sizeof(physical), 0x01,
        37, -23, 5, 0, report, sizeof(report)) == 7);
    assert(report[0] == 0x04);
    assert(report[1] == 0x03);
    assert(report[2] == 37 && report[3] == 0);
    assert(report[4] == 0xE9 && report[5] == 0xFF);
    assert(report[6] == 5);
    assert(makcm::hid::readMouseButtons(layout, report, 7) == 0x03);

    // 1000 Hz producers may add more relative motion than one 16-bit report
    // can carry.  Preserve the exact integer sum across USB polls.
    int64_t pendingX = 40000;
    int64_t pendingY = -40000;
    int64_t pendingWheel = 0;
    int64_t pendingPan = 0;
    makcm::hid::MouseMergeDelta consumed;
    assert(makcm::hid::mergeMouseReport(
        layout, nullptr, 0, false, 0, pendingX, pendingY,
        pendingWheel, pendingPan, report, sizeof(report), &consumed) == 7);
    assert(consumed.x == 32767 && consumed.y == -32767);
    assert(pendingX == 7233 && pendingY == -7233);
    assert(report[2] == 0xFF && report[3] == 0x7F);
    assert(report[4] == 0x01 && report[5] == 0x80);
    assert(makcm::hid::mergeMouseReport(
        layout, nullptr, 0, false, 0, pendingX, pendingY,
        pendingWheel, pendingPan, report, sizeof(report), &consumed) == 7);
    assert(consumed.x == 7233 && consumed.y == -7233);
    assert(pendingX == 0 && pendingY == 0);

    // Merge, rather than replace, physical relative movement and preserve
    // trailing bytes used by padded reports.
    const uint8_t paddedPhysical[] = {
        0x04, 0x02, 0x0A, 0x00, 0xFB, 0xFF, 0x01, 0xA5, 0x5A
    };
    pendingX = 7;
    pendingY = -8;
    pendingWheel = 2;
    pendingPan = 0;
    assert(makcm::hid::mergeMouseReport(
        layout, paddedPhysical, sizeof(paddedPhysical), true, 0x01,
        pendingX, pendingY, pendingWheel, pendingPan,
        report, sizeof(report), &consumed) == sizeof(paddedPhysical));
    assert(report[1] == 0x03);  // physical RMB + injected LMB
    assert(report[2] == 17 && report[3] == 0);
    assert(report[4] == 0xF3 && report[5] == 0xFF); // -5 + -8 = -13
    assert(report[6] == 3);
    assert(report[7] == 0xA5 && report[8] == 0x5A);
    assert(pendingX == 0 && pendingY == 0 && pendingWheel == 0);

    const uint8_t bootDescriptor[] = {
        0x05,0x01,0x09,0x02,0xA1,0x01,0x09,0x01,0xA1,0x00,
        0x05,0x09,0x19,0x01,0x29,0x03,0x15,0x00,0x25,0x01,
        0x95,0x03,0x75,0x01,0x81,0x02,0x95,0x01,0x75,0x05,
        0x81,0x01,0x05,0x01,0x09,0x30,0x09,0x31,0x09,0x38,
        0x15,0x81,0x25,0x7F,0x75,0x08,0x95,0x03,0x81,0x06,
        0xC0,0xC0,
    };
    assert(makcm::hid::parseMouseReportDescriptor(
        bootDescriptor, sizeof(bootDescriptor), layout));
    assert(!layout.hasReportId && layout.reportLength == 4);
    assert(makcm::hid::composeMouseReport(
        layout, nullptr, 0, 0, -7, 9, 0, 0,
        report, sizeof(report)) == 4);
    assert(report[1] == static_cast<uint8_t>(-7));
    assert(report[2] == 9);

    pendingX = 300;
    pendingY = 0;
    pendingWheel = pendingPan = 0;
    int64_t emittedTotal = 0;
    while (pendingX != 0) {
        assert(makcm::hid::mergeMouseReport(
            layout, nullptr, 0, false, 0, pendingX, pendingY,
            pendingWheel, pendingPan, report, sizeof(report), &consumed) == 4);
        emittedTotal += consumed.x;
    }
    assert(emittedTotal == 300);

    // A descriptor-invalid physical value must be preserved and must not
    // manufacture an opposite injected contribution.
    const auto boot = makcm::hid::bootMouseLayout();
    const uint8_t invalidPhysical[4] = {0, 0x80, 0, 0};
    pendingX = pendingY = pendingWheel = pendingPan = 0;
    makcm::hid::MouseMergeDelta invalidConsumed;
    assert(makcm::hid::mergeMouseReport(
        boot, invalidPhysical, sizeof(invalidPhysical), true, 0,
        pendingX, pendingY, pendingWheel, pendingPan,
        report, sizeof(report), &invalidConsumed) == 4);
    assert(report[1] == 0x80);
    assert(invalidConsumed.x == 0 && pendingX == 0);

    uint8_t saturatedPhysical[4] = {0, 120, 0, 0};
    pendingX = 20;
    assert(makcm::hid::mergeMouseReport(
        boot, saturatedPhysical, sizeof(saturatedPhysical), true, 0,
        pendingX, pendingY, pendingWheel, pendingPan,
        report, sizeof(report), &consumed) == 4);
    assert(static_cast<int8_t>(report[1]) == 127);
    assert(consumed.x == 7 && pendingX == 13);

    saturatedPhysical[1] = 127;
    pendingX = 5;
    assert(makcm::hid::mergeMouseReport(
        boot, saturatedPhysical, sizeof(saturatedPhysical), true, 0,
        pendingX, pendingY, pendingWheel, pendingPan,
        report, sizeof(report), &consumed) == 4);
    assert(consumed.x == 0 && pendingX == 5);

    saturatedPhysical[1] = 120;
    pendingX = -300;
    assert(makcm::hid::mergeMouseReport(
        boot, saturatedPhysical, sizeof(saturatedPhysical), true, 0,
        pendingX, pendingY, pendingWheel, pendingPan,
        report, sizeof(report), &consumed) == 4);
    assert(static_cast<int8_t>(report[1]) == -127);
    assert(consumed.x == -247 && pendingX == -53);
    return 0;
}
