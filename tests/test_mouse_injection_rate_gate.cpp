#include "MouseInjectionRateGate.h"

#include <assert.h>

using makcm::hid::MouseInjectionRateGate;

int main() {
    MouseInjectionRateGate gate;

    // Normal firmware behavior is enabled and unlimited.
    assert(gate.enabled());
    assert(gate.rateHz() == 0);
    assert(gate.allowed(0));
    gate.noteSubmit(100);
    assert(gate.allowed(101));

    gate.configure(false, 500);
    assert(!gate.enabled());
    assert(gate.rateHz() == 0);
    assert(!gate.allowed(1000000));

    gate.configure(true, 125);
    assert(gate.intervalUs() == 8000);
    assert(gate.allowed(1000));
    gate.noteSubmit(1000);
    assert(!gate.allowed(8999));
    assert(gate.allowed(9000));

    gate.configure(true, 250);
    assert(gate.intervalUs() == 4000);
    assert(gate.allowed(0));
    gate.noteSubmit(5000);
    assert(!gate.allowed(8999));
    assert(gate.allowed(9000));

    gate.configure(true, 500);
    assert(gate.intervalUs() == 2000);
    gate.noteSubmit(UINT64_MAX - 1000);
    // Unsigned subtraction keeps the comparison correct across wraparound.
    assert(!gate.allowed(998));
    assert(gate.allowed(999));

    gate.configure(true, 0);
    gate.noteSubmit(UINT64_MAX);
    assert(gate.allowed(0));

    return 0;
}
