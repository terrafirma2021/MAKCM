#include "BridgeSequenceTracker.h"

#include <cassert>

using makcm::bridge::InputSequenceTracker;

int main() {
    InputSequenceTracker tracker;

    tracker.observe(0x11, 0xFFFE);
    tracker.observe(0x11, 0xFFFF);
    tracker.observe(0x11, 0x0000); // normal uint16 wrap
    assert(tracker.frames() == 3);
    assert(tracker.gapFrames() == 0);
    assert(tracker.gapEvents() == 0);

    tracker.observe(0x11, 0x0002); // sequence 1 is missing
    assert(tracker.gapFrames() == 1);
    assert(tracker.gapEvents() == 1);

    tracker.observe(0x11, 0x0002); // duplicate of the last accepted frame
    assert(tracker.duplicates() == 1);
    assert(tracker.outOfOrder() == 1);

    tracker.observe(0x11, 0x0001); // an older, out-of-order frame
    assert(tracker.duplicates() == 1);
    assert(tracker.outOfOrder() == 2);

    tracker.observe(0x22, 0x0042); // new USB session starts a fresh stream
    assert(tracker.sessionChanges() == 1);
    assert(tracker.gapFrames() == 1);

    tracker.reset();
    assert(tracker.frames() == 0);
    assert(tracker.gapFrames() == 0);
    assert(tracker.duplicates() == 0);
    return 0;
}
