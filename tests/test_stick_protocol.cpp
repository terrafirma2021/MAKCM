#include "StickProtocol.h"
#include "StickSession.h"

#include <cassert>
#include <cstdint>
#include <cstring>

using namespace makcm::stick;

int main() {
    char wire[160]{};
    const size_t wireLength = encodeStickState(7, 0xFFFFFFFEu, 1234, -2345,
                                               32, wire, sizeof(wire));
    assert(wireLength != 0);
    StickCommand parsed;
    StickParseError parseError = StickParseError::None;
    assert(parseStickCommand(reinterpret_cast<const uint8_t *>(wire), wireLength,
                              parsed, &parseError));
    assert(parseError == StickParseError::None);
    assert(parsed.injectionSession == 7 && parsed.sequence == 0xFFFFFFFEu);
    assert(parsed.x == 1234 && parsed.y == -2345 && parsed.ttlMs == 32);
    assert(parsed.source == StickSource::StateQ15);

    // CRC mismatch is rejected before any command fields are consumed.
    char corrupted[160]{};
    std::memcpy(corrupted, wire, wireLength);
    corrupted[wireLength - 7] ^= 0x01;
    assert(!parseStickCommand(reinterpret_cast<const uint8_t *>(corrupted), wireLength,
                              parsed, &parseError));
    assert(parseError == StickParseError::CrcMismatch);

    // The board's TTL ceiling is fail-closed and does not accept zero/overlong
    // leases.  The parser accepts hexadecimal sessions and sequence numbers.
    const char invalidTtl[] = "km.stick.state(0x7,1,0,0,81)*0000\n";
    assert(!parseStickCommand(invalidTtl, sizeof(invalidTtl) - 1, parsed,
                              &parseError));
    assert(parseError == StickParseError::CrcMismatch ||
           parseError == StickParseError::InvalidTtl);

    assert(checkSequence(false, 0, 42).disposition == SequenceDisposition::First);
    assert(checkSequence(true, 9, 9).disposition == SequenceDisposition::Duplicate);
    assert(checkSequence(true, 9, 8).disposition == SequenceDisposition::Stale);
    const auto gap = checkSequence(true, 9, 12);
    assert(gap.disposition == SequenceDisposition::NewWithGap && gap.gap == 2);
    assert(checkSequence(true, 0xFFFFFFFFu, 0).disposition == SequenceDisposition::New);
    assert(checkSequence(true, 0, 0x80000000u).disposition == SequenceDisposition::Stale);

    StickSession session;
    assert(session.bindPhysical(0x11223344u));
    assert(session.beginInjection(0x11223344u, 7, StickSource::StateQ15, 100));
    StickCommand command;
    command.kind = StickCommandKind::State;
    command.source = StickSource::StateQ15;
    command.injectionSession = 7;
    command.sequence = 0xFFFFFFFEu;
    command.x = 2000;
    command.y = -3000;
    command.ttlMs = 32;
    auto accepted = session.accept(command, 0x11223344u, 101);
    assert(accepted.accepted);
    auto sample = session.sample(120);
    assert(!sample.expired && sample.x == 2000 && sample.y == -3000);

    // Duplicate/stale commands cannot refresh the TTL.
    command.x = 777;
    assert(!session.accept(command, 0x11223344u, 121).accepted);
    assert(session.counters().duplicateSequence == 1);
    command.sequence = 0xFFFFFFFDu;
    assert(!session.accept(command, 0x11223344u, 122).accepted);
    assert(session.counters().staleSequence == 1);
    assert(session.sample(133).expired);
    assert(session.sample(134).x == 0 && session.sample(134).y == 0);
    assert(session.counters().ttlExpiry == 1);

    // Physical-session mismatch is a hard fail-closed boundary.  A source
    // change is rejected until a fresh injection session is explicitly bound.
    session.reset();
    assert(session.bindPhysical(10));
    assert(session.beginInjection(10, 1, StickSource::StateQ15));
    command.injectionSession = 1;
    command.sequence = 1;
    command.source = StickSource::StateQ15;
    command.ttlMs = 32;
    assert(!session.accept(command, 11, 0).accepted);
    assert(session.state() == StickState::Fault);
    assert(session.sample(1).x == 0 && session.sample(1).y == 0);

    session.reset();
    assert(session.bindPhysical(10));
    assert(session.beginInjection(10, 1, StickSource::StateQ15));
    command.physicalSession = 0;
    assert(session.accept(command, 10, 0).accepted);
    command.sequence = 2;
    command.source = StickSource::Rate;
    command.kind = StickCommandKind::Rate;
    assert(!session.accept(command, 10, 1).accepted);
    assert(session.counters().sourceConflict == 1);
    return 0;
}
