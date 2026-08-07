#include "EndpointTxPhase.h"

#include <assert.h>

using makcm::usb::EndpointCompletion;
using makcm::usb::EndpointPhase;
using makcm::usb::EndpointTxPhase;

int main() {
    EndpointTxPhase mouse;
    EndpointTxPhase xinput;
    assert(mouse.phase() == EndpointPhase::Closed);
    mouse.open(7);
    xinput.open(7);

    assert(mouse.begin(7));
    assert(!mouse.begin(7));
    assert(mouse.arm(7, 7));
    // Mouse in flight cannot affect an independent XInput gate.
    assert(xinput.begin(7));
    assert(xinput.arm(7, 20));
    assert(xinput.beginCompletion(7, true, 20) ==
           EndpointCompletion::Delivered);
    assert(xinput.finishCompletion(7));

    // Endpoint MPS may be 64 while the submitted report itself is 7 bytes.
    assert(mouse.beginCompletion(7, true, 7) ==
           EndpointCompletion::Delivered);
    assert(mouse.finishCompletion(7));

    assert(mouse.begin(7));
    assert(mouse.arm(7, 7));
    assert(mouse.beginCompletion(7, true, 6) == EndpointCompletion::Retry);
    assert(mouse.finishCompletion(7));

    assert(mouse.begin(7));
    assert(mouse.arm(7, 7));
    assert(mouse.submitFailed(7));
    assert(mouse.phase() == EndpointPhase::Idle);

    // Completion can happen before the submit call returns; the later failure
    // path must not resurrect or alter that completed ticket.
    assert(mouse.begin(7));
    assert(mouse.arm(7, 7));
    assert(mouse.beginCompletion(7, true, 7) ==
           EndpointCompletion::Delivered);
    assert(mouse.finishCompletion(7));
    assert(!mouse.submitFailed(7));

    // Reset/open advances the epoch. Old callbacks are stale.
    assert(mouse.begin(7));
    assert(mouse.arm(7, 7));
    mouse.reset();
    mouse.open(8);
    assert(mouse.beginCompletion(7, true, 7) == EndpointCompletion::Stale);

    // OUT receives are armed for MPS but a shorter host packet is valid.
    EndpointTxPhase output;
    output.open(8);
    assert(output.begin(8));
    assert(output.arm(8, 64));
    assert(output.beginCompletion(8, true, 16, false) ==
           EndpointCompletion::Delivered);
    assert(output.finishCompletion(8));

    return 0;
}
