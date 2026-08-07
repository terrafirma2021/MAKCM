#include "MouseInjectionLedger.h"

#include <assert.h>
#include <initializer_list>
#include <stdint.h>

using makcm::hid::MouseInjectionLedger;
using makcm::hid::MouseMergeDelta;

static void stageX(MouseInjectionLedger &ledger, int64_t amount) {
    const auto snapshot = ledger.snapshot();
    MouseMergeDelta delivered;
    delivered.x = amount;
    assert(ledger.reserve(delivered, snapshot.buttonGeneration));
}

int main() {
    MouseInjectionLedger ledger;

    assert(ledger.add(40, -50, 0, 0, false, false));
    auto snapshot = ledger.snapshot();
    assert(snapshot.x == 40 && snapshot.y == -50);
    assert(!ledger.add(1, 2, 3, 0, false, false));
    snapshot = ledger.snapshot();
    assert(snapshot.x == 40 && snapshot.y == -50 && snapshot.wheel == 0);

    ledger.reset();
    assert(ledger.add(INT64_MAX, INT64_MIN, 0, 0, false, false));
    assert(!ledger.add(1, 0, 0, 0, false, false));
    assert(!ledger.add(0, -1, 0, 0, false, false));
    snapshot = ledger.snapshot();
    assert(snapshot.x == INT64_MAX && snapshot.y == INT64_MIN);

    // Submission/completion failure leaves the entire contribution pending.
    ledger.reset();
    assert(ledger.add(300, 0, 0, 0, false, false));
    stageX(ledger, 127);
    assert(ledger.settle(false));
    assert(ledger.snapshot().x == 300);

    int64_t deliveredTotal = 0;
    for (const int64_t part : {127LL, 127LL, 46LL}) {
        stageX(ledger, part);
        assert(ledger.settle(true));
        deliveredTotal += part;
        assert(deliveredTotal + ledger.snapshot().x == 300);
    }
    assert(ledger.snapshot().x == 0);

    // Opposite-sign producer updates during an active report preserve the net
    // sum and reject the only extreme value that would make success overflow.
    ledger.reset();
    assert(ledger.add(100, 0, 0, 0, false, false));
    stageX(ledger, 100);
    assert(ledger.add(-100, 0, 0, 0, false, false));
    assert(!ledger.add(INT64_MIN, 0, 0, 0, false, false));
    assert(ledger.settle(true));
    assert(ledger.snapshot().x == -100);

    ledger.reset();
    assert(ledger.add(100, 0, 0, 0, false, false));
    stageX(ledger, 100);
    assert(ledger.add(-100, 0, 0, 0, false, false));
    assert(ledger.settle(false));
    assert(ledger.snapshot().x == 0);

    // Buttons are an idempotent latch with completion generations.
    ledger.reset();
    assert(ledger.setButton(0x01, true));
    assert(!ledger.setButton(0x01, true));
    snapshot = ledger.snapshot();
    assert(snapshot.buttons == 0x01 && snapshot.buttonGeneration == 1);
    MouseMergeDelta none;
    assert(ledger.reserve(none, snapshot.buttonGeneration));
    assert(ledger.setButton(0x01, false));
    assert(ledger.settle(true));
    snapshot = ledger.snapshot();
    assert(snapshot.buttons == 0 && snapshot.buttonSentGeneration == 1);
    assert(ledger.pending());
    assert(ledger.reserve(none, snapshot.buttonGeneration));
    assert(ledger.settle(true));
    assert(!ledger.pending());

    // Bus reset keeps a held state and makes it dirty for the newly enumerated
    // host. A released, fully delivered state stays quiet.
    ledger.reset();
    assert(ledger.setButton(0x02, true));
    snapshot = ledger.snapshot();
    assert(ledger.reserve(none, snapshot.buttonGeneration));
    assert(ledger.settle(true));
    ledger.markHostReset();
    assert(ledger.pending() && ledger.snapshot().buttons == 0x02);
    assert(ledger.setButton(0x02, false));
    snapshot = ledger.snapshot();
    assert(ledger.reserve(none, snapshot.buttonGeneration));
    assert(ledger.settle(true));
    assert(!ledger.pending());
    ledger.markHostReset();
    assert(!ledger.pending());

    // Fixed-seed stress: every success changes the delivered side exactly once;
    // failures leave it pending. Add another command while each report is active
    // to exercise the success/failure representability checks.
    ledger.reset();
    uint32_t random = 0xC001D00Du;
    __int128 accepted = 0;
    __int128 delivered = 0;
    for (int i = 0; i < 10000; ++i) {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        const int64_t delta = static_cast<int32_t>(random % 401) - 200;
        assert(ledger.add(delta, 0, 0, 0, false, false));
        accepted += delta;

        snapshot = ledger.snapshot();
        const int64_t part = snapshot.x < -127 ? -127 :
            (snapshot.x > 127 ? 127 : snapshot.x);
        stageX(ledger, part);

        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        const int64_t during = static_cast<int32_t>(random % 101) - 50;
        assert(ledger.add(during, 0, 0, 0, false, false));
        accepted += during;

        const bool success = (random & 3u) != 0;
        assert(ledger.settle(success));
        if (success) delivered += part;
        assert(accepted == delivered + ledger.snapshot().x);
    }
    while (ledger.snapshot().x != 0) {
        const int64_t pending = ledger.snapshot().x;
        const int64_t part = pending < -127 ? -127 :
            (pending > 127 ? 127 : pending);
        stageX(ledger, part);
        assert(ledger.settle(true));
        delivered += part;
    }
    assert(accepted == delivered);

    return 0;
}
