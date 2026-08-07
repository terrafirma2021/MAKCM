#include "stick_runtime.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>

using namespace makcm::native;
using namespace makcm::stick;

namespace {
ControllerProfile testProfile(ControllerProfileIdentity &identity) {
    identity.vid = 0x054C;
    identity.pid = 0x0DF2;
    identity.bcdDevice = 0x0200;
    identity.descriptorHash = 0xA1B2C3D4;
    identity.hidReportHash = 0x11223344;
    identity.interfaceNumber = 3;
    identity.endpointAddress = 0x84;
    identity.reportId = 1;
    identity.reportLength = 8;
    identity.formatVersion = 1;
    identity.variant = 7;
    return makeDualSenseUsbUint8Profile(
        identity, uint8Axis(3, 128, 0, 255),
        uint8Axis(4, 128, 0, 255, -1));
}
} // namespace

int main() {
    StickRuntime runtime;
    runtime.bindPhysical(0x12345678u, 1);
    ControllerProfileIdentity identity;
    const auto profile = testProfile(identity);

    // Keep the bring-up sequence reusable for the physical-priority edge
    // cases below.  The production path deliberately requires two delivered
    // raw reports before acknowledging begin().
    auto armStick = [&](StickRuntime &candidate, uint32_t physical_session,
                        uint64_t now_ms,
                        const ControllerProfile &candidate_profile) {
        char arm_response[640]{};
        candidate.bindPhysical(physical_session, now_ms);
        assert(candidate.configureExactProfile("dse-capture-v1", "none",
                                               candidate_profile, identity));
        assert(candidate.handleLine("km.stick.profile(hip,none)", now_ms,
                                    arm_response, sizeof(arm_response)));
        assert(candidate.handleLine(
            "km.stick.begin(dse-capture-v1,none,state_q15)", now_ms + 1,
            arm_response, sizeof(arm_response)));
        const uint8_t arm_original[8] =
            {1, 0x11, 0x22, 128, 128, 0x55, 0x66, 0x77};
        uint8_t arm_compose[8]{};
        std::memcpy(arm_compose, arm_original, sizeof(arm_compose));
        auto arm_result = candidate.compose(0x84, arm_original, arm_compose,
                                            sizeof(arm_compose), now_ms + 2);
        candidate.reportCompleted(0x84, true, arm_result, now_ms + 2);
        std::memcpy(arm_compose, arm_original, sizeof(arm_compose));
        arm_result = candidate.compose(0x84, arm_original, arm_compose,
                                       sizeof(arm_compose), now_ms + 3);
        candidate.reportCompleted(0x84, true, arm_result, now_ms + 3);
        assert(candidate.takePendingResponse(arm_response,
                                             sizeof(arm_response)));
        assert(candidate.handleLine("km.output_mode(stick)", now_ms + 4,
                                    arm_response, sizeof(arm_response)));
        return candidate.snapshot(now_ms + 4).injection_session;
    };
    assert(runtime.configureExactProfile("dse-capture-v1", "none", profile,
                                         identity));

    char response[kStickStatusCapacity]{};
    assert(runtime.handleLine("km.caps()", 1, response, sizeof(response)));
    assert(std::strstr(response, "STICK_CAPS(v=2") != nullptr);
    assert(std::strstr(response, "game_cal=0") != nullptr);
    assert(runtime.handleLine("km.stick.profile(ads,none)", 1, response,
                              sizeof(response)));
    assert(std::strstr(response, "STICK_ACK(profile") != nullptr);
    assert(runtime.handleLine("km.stick.profile(hip,wrong)", 1, response,
                              sizeof(response)));
    assert(std::strstr(response, "calibration_mismatch") != nullptr);
    assert(runtime.handleLine("km.stick.profile(hip,none)", 1, response,
                              sizeof(response)));
    assert(runtime.handleLine(
        "km.stick.begin(dse-capture-v1,none,state_q15)", 2,
        response, sizeof(response)));
    assert(response[0] == '\0'); // begin ACK waits for completed raw reports.

    // A missing/mismatched report stream cannot leave an injection session
    // resident forever.  The board owns this fail-close timeout independently
    // of any host harness timeout.
    StickRuntime begin_timeout;
    begin_timeout.bindPhysical(0xABCDEF01u, 1);
    assert(begin_timeout.configureExactProfile("dse-capture-v1", "none",
                                               profile, identity));
    assert(begin_timeout.handleLine(
        "km.stick.begin(dse-capture-v1,none,state_q15)", 2,
        response, sizeof(response)));
    begin_timeout.checkStreamWatchdog(101);
    assert(!begin_timeout.takePendingResponse(response, sizeof(response)));
    begin_timeout.checkStreamWatchdog(102);
    assert(begin_timeout.takePendingResponse(response, sizeof(response)));
    assert(std::strstr(response,
                       "STICK_NACK(begin,barrier_timeout,raw_safe)") != nullptr);
    auto timeout_snapshot = begin_timeout.snapshot(102);
    assert(timeout_snapshot.state == StickState::RawSafe);
    assert(timeout_snapshot.source == StickSource::None);
    assert(timeout_snapshot.injection_session == 0);
    assert(timeout_snapshot.mode == StickOutputMode::Mouse);
    assert(timeout_snapshot.counters.barrier_timeouts == 1);
    assert(!begin_timeout.takeDetachRequest());
    assert(begin_timeout.handleLine("km.output_mode(mouse)", 103, response,
                                    sizeof(response)));
    assert(std::strstr(response, "STICK_ACK(output_mode") != nullptr);

    const uint8_t original[8] = {1, 0x11, 0x22, 128, 128, 0x55, 0x66, 0x77};
    uint8_t compose[8]{};
    std::memcpy(compose, original, sizeof(compose));
    auto result = runtime.compose(0x84, original, compose, sizeof(compose), 3);
    assert(result.qualifying_report && !result.modified);
    assert(std::memcmp(original, compose, sizeof(original)) == 0);
    runtime.reportCompleted(0x84, true, result, 4);
    result = runtime.compose(0x84, original, compose, sizeof(compose), 5);
    runtime.reportCompleted(0x84, true, result, 6);
    assert(runtime.takePendingResponse(response, sizeof(response)));

    unsigned long physical = 0;
    unsigned long injection = 0;
    char mode[16]{};
    char format[65]{};
    char cal[65]{};
    assert(std::sscanf(response, "STICK_ACK(begin,%lu,%lu,%15[^,],%64[^,],%64[^)])",
                       &physical, &injection, mode, format, cal) == 5);
    assert(physical == 0x12345678u && injection != 0);
    assert(std::strcmp(mode, "mouse") == 0);

    assert(runtime.handleLine("km.output_mode(stick)", 7, response,
                              sizeof(response)));
    assert(std::strstr(response, "STICK_ACK(output_mode") != nullptr);

    char state_line[128]{};
    const size_t encoded = encodeStickState(
        static_cast<uint32_t>(injection), 1, 11468, 0, 32,
        state_line, sizeof(state_line));
    assert(encoded != 0);
    assert(runtime.handleLine(state_line, 10, response, sizeof(response)));
    assert(response[0] == '\0');

    std::memcpy(compose, original, sizeof(compose));
    result = runtime.compose(0x84, original, compose, sizeof(compose), 11);
    assert(result.modified);
    // Phase-3 firmware clamps the requested 0.35 vector to 0.20.
    assert(compose[3] == 153 && compose[4] == 128);
    assert(original[3] == 128 && original[4] == 128);
    for (size_t i : {size_t(0), size_t(1), size_t(2), size_t(5),
                     size_t(6), size_t(7)}) {
        assert(compose[i] == original[i]);
    }
    runtime.reportCompleted(0x84, false, result, 11);
    auto snapshot = runtime.snapshot(11);
    assert(snapshot.counters.reports_modified == 0);
    assert(snapshot.synthetic_x == 0 && snapshot.synthetic_y == 0);
    std::memcpy(compose, original, sizeof(compose));
    result = runtime.compose(0x84, original, compose, sizeof(compose), 12);
    runtime.reportCompleted(0x84, true, result, 12);
    snapshot = runtime.snapshot(12);
    assert(snapshot.counters.reports_modified == 1);
    assert(snapshot.synthetic_x != 0);
    assert(runtime.handleLine("km.stick.status()", 12, response,
                              sizeof(response)));
    assert(std::strstr(response, "timing=1/10/1/10/12") != nullptr);

    // A sign reversal must emit one same-axis center report. A failed USB
    // completion does not consume that barrier.
    assert(encodeStickState(static_cast<uint32_t>(injection), 2, -11468, 0, 32,
                            state_line, sizeof(state_line)) != 0);
    assert(runtime.handleLine(state_line, 13, response, sizeof(response)));
    std::memcpy(compose, original, sizeof(compose));
    result = runtime.compose(0x84, original, compose, sizeof(compose), 13);
    assert(result.reversal_epoch_x != 0 && result.synthetic_x == 0);
    runtime.reportCompleted(0x84, false, result, 13);
    result = runtime.compose(0x84, original, compose, sizeof(compose), 14);
    assert(result.reversal_epoch_x != 0 && result.synthetic_x == 0);
    runtime.reportCompleted(0x84, true, result, 14);
    snapshot = runtime.snapshot(14);
    assert(snapshot.counters.direction_reversal_centers == 1);
    result = runtime.compose(0x84, original, compose, sizeof(compose), 15);
    assert(result.reversal_epoch_x == 0 && result.synthetic_x < 0);
    runtime.reportCompleted(0x84, true, result, 15);

    // At TTL expiry synthetic state centers logically. The runtime leaves the
    // fresh physical report untouched instead of replaying or fabricating it.
    auto pre_expiry_snapshot = runtime.snapshot(45);
    auto pre_expiry_again = runtime.snapshot(45);
    assert(pre_expiry_snapshot.state == StickState::Expired);
    assert(pre_expiry_again.state == StickState::Expired);
    assert(pre_expiry_snapshot.session_counters.ttlExpiry == 0);
    assert(pre_expiry_again.session_counters.ttlExpiry == 0);
    const uint8_t ttl_jitter[8] =
        {1, 0x11, 0x22, 132, 128, 0x55, 0x66, 0x77};
    std::memcpy(compose, ttl_jitter, sizeof(compose));
    result = runtime.compose(0x84, ttl_jitter, compose, sizeof(compose), 45);
    assert(result.forced_center && !result.modified);
    assert(std::memcmp(ttl_jitter, compose, sizeof(ttl_jitter)) == 0);
    runtime.reportCompleted(0x84, true, result, 45);
    snapshot = runtime.snapshot(45);
    assert(snapshot.counters.reports_forced_center == 1);

    // An out-of-profile raw field remains fail-closed, but is visible through
    // the dedicated decode counter and STICK_STATUS telemetry.
    ControllerProfile narrow_profile = profile;
    narrow_profile.x.minimum = 10;
    narrow_profile.x.maximum = 240;
    narrow_profile.y.minimum = 10;
    narrow_profile.y.maximum = 240;
    StickRuntime decode_runtime;
    const uint32_t decode_injection =
        armStick(decode_runtime, 0xABCDEF09u, 1, narrow_profile);
    assert(encodeStickState(decode_injection, 1, 11468, 0, 32, state_line,
                            sizeof(state_line)) != 0);
    assert(decode_runtime.handleLine(state_line, 10, response,
                                     sizeof(response)));
    const uint8_t invalid_raw[8] =
        {1, 0x11, 0x22, 0, 128, 0x55, 0x66, 0x77};
    uint8_t invalid_compose[8]{};
    std::memcpy(invalid_compose, invalid_raw, sizeof(invalid_compose));
    result = decode_runtime.compose(0x84, invalid_raw, invalid_compose,
                                    sizeof(invalid_compose), 11);
    assert(result.qualifying_report && !result.modified);
    assert(result.codec_error == CodecError::InvalidField);
    assert(std::memcmp(invalid_raw, invalid_compose, sizeof(invalid_raw)) == 0);
    auto decode_snapshot = decode_runtime.snapshot(11);
    assert(decode_snapshot.counters.codec_decode_error == 1);
    assert(decode_runtime.handleLine("km.stick.status()", 11, response,
                                     sizeof(response)));
    assert(std::strstr(response, "decode=1") != nullptr);

    // Long but valid profile tokens must produce a complete status in the
    // unified capacity, while a short caller receives an explicit truncated
    // NACK rather than a partial STICK_STATUS frame.  The production 640-byte
    // caller is covered by the same return-value branch; the max-counter
    // length bound is checked separately by the host-side status parser.
    char long_format[65]{};
    char long_cal[65]{};
    std::memset(long_format, 'f', sizeof(long_format) - 1);
    std::memset(long_cal, 'c', sizeof(long_cal) - 1);
    StickRuntime long_status;
    long_status.bindPhysical(0xABCDEF0Au, 1);
    assert(long_status.configureExactProfile(long_format, long_cal, profile,
                                             identity));
    char short_status[128]{};
    assert(long_status.handleLine("km.stick.status()", 2, short_status,
                                  sizeof(short_status)));
    assert(std::strstr(short_status,
                       "STICK_NACK(status,truncated,raw_safe)") != nullptr);
    char complete_status[kStickStatusCapacity]{};
    assert(long_status.handleLine("km.stick.status()", 2, complete_status,
                                  sizeof(complete_status)));
    assert(std::strncmp(complete_status, "STICK_STATUS(", 13) == 0);
    assert(std::strstr(complete_status, long_format) != nullptr);
    assert(std::strstr(complete_status, long_cal) != nullptr);
    assert(std::strstr(complete_status, "decode=0") != nullptr);

    // In the default neutral-only hysteresis band (.06 <= |physical| < .10),
    // merge() selects physical and the FIFO image must remain byte-for-byte
    // unchanged even while synthetic state is active.
    StickRuntime band_runtime;
    const uint32_t band_injection =
        armStick(band_runtime, 0xBEEF0001u, 1, profile);
    assert(encodeStickState(band_injection, 1, 11468, 0, 32, state_line,
                            sizeof(state_line)) != 0);
    assert(band_runtime.handleLine(state_line, 10, response, sizeof(response)));
    const uint8_t band_original[8] =
        {1, 0x11, 0x22, 138, 128, 0x55, 0x66, 0x77};
    uint8_t band_compose[8]{};
    std::memcpy(band_compose, band_original, sizeof(band_compose));
    auto band_result = band_runtime.compose(
        0x84, band_original, band_compose, sizeof(band_compose), 11);
    assert(band_result.qualifying_report && !band_result.modified);
    assert(!band_result.physical_override && !band_result.awaiting_fresh);
    assert(std::memcmp(band_original, band_compose, sizeof(band_original)) ==
           0);
    band_runtime.reportCompleted(0x84, true, band_result, 11);
    auto band_snapshot = band_runtime.snapshot(11);
    assert(!band_snapshot.physical_override && !band_snapshot.awaiting_fresh);

    // A latched override ends immediately after a stable physical release;
    // awaitingFresh has independent state/counters and does not inflate the
    // active physical-override report count or duration.
    StickRuntime latch_runtime;
    const uint32_t latch_injection =
        armStick(latch_runtime, 0xBEEF0002u, 1, profile);
    assert(encodeStickState(latch_injection, 1, 11468, 0, 32, state_line,
                            sizeof(state_line)) != 0);
    assert(latch_runtime.handleLine(state_line, 9, response, sizeof(response)));
    const uint8_t latch_high[8] =
        {1, 0x11, 0x22, 155, 128, 0x55, 0x66, 0x77};
    const uint8_t latch_center[8] =
        {1, 0x11, 0x22, 128, 128, 0x55, 0x66, 0x77};
    uint8_t latch_compose[8]{};
    std::memcpy(latch_compose, latch_high, sizeof(latch_compose));
    auto latch_result = latch_runtime.compose(
        0x84, latch_high, latch_compose, sizeof(latch_compose), 10);
    assert(latch_result.physical_override && !latch_result.awaiting_fresh);
    latch_runtime.reportCompleted(0x84, true, latch_result, 10);
    auto latch_snapshot = latch_runtime.snapshot(10);
    assert(latch_snapshot.physical_override);
    assert(latch_snapshot.counters.physical_override_count == 1);
    assert(latch_snapshot.counters.physical_override_reports == 1);

    std::memcpy(latch_compose, latch_center, sizeof(latch_compose));
    latch_result = latch_runtime.compose(
        0x84, latch_center, latch_compose, sizeof(latch_compose), 20);
    assert(latch_result.physical_override && !latch_result.awaiting_fresh);
    latch_runtime.reportCompleted(0x84, true, latch_result, 20);
    std::memcpy(latch_compose, latch_center, sizeof(latch_compose));
    latch_result = latch_runtime.compose(
        0x84, latch_center, latch_compose, sizeof(latch_compose), 40);
    assert(!latch_result.physical_override && latch_result.awaiting_fresh);
    latch_runtime.reportCompleted(0x84, true, latch_result, 40);
    latch_snapshot = latch_runtime.snapshot(40);
    assert(latch_snapshot.state == StickState::StickIdle);
    assert(!latch_snapshot.physical_override && latch_snapshot.awaiting_fresh);
    assert(latch_snapshot.counters.physical_override_count == 1);
    assert(latch_snapshot.counters.physical_override_reports == 2);
    assert(latch_snapshot.counters.physical_override_ms == 30);
    assert(latch_snapshot.counters.awaiting_fresh_count == 1);
    assert(latch_snapshot.counters.awaiting_fresh_reports == 1);

    std::memcpy(latch_compose, latch_center, sizeof(latch_compose));
    latch_result = latch_runtime.compose(
        0x84, latch_center, latch_compose, sizeof(latch_compose), 45);
    assert(!latch_result.physical_override && latch_result.awaiting_fresh);
    latch_runtime.reportCompleted(0x84, true, latch_result, 45);
    latch_snapshot = latch_runtime.snapshot(45);
    assert(latch_snapshot.counters.physical_override_reports == 2);
    assert(latch_snapshot.counters.physical_override_ms == 30);
    assert(latch_snapshot.counters.awaiting_fresh_reports == 2);
    assert(latch_snapshot.counters.awaiting_fresh_ms == 5);
    assert(latch_runtime.handleLine("km.stick.status()", 45, response,
                                    sizeof(response)));
    assert(std::strstr(response, ",stick_idle,") != nullptr);
    assert(std::strstr(response, "awaiting=1/1/2/5") != nullptr);

    // A new sequence clears only awaitingFresh.  The accepted non-zero
    // command restores synthetic output and leaves the session active.
    assert(encodeStickState(latch_injection, 2, 11468, 0, 32, state_line,
                            sizeof(state_line)) != 0);
    assert(latch_runtime.handleLine(state_line, 50, response, sizeof(response)));
    std::memcpy(latch_compose, latch_center, sizeof(latch_compose));
    latch_result = latch_runtime.compose(
        0x84, latch_center, latch_compose, sizeof(latch_compose), 51);
    assert(!latch_result.physical_override && !latch_result.awaiting_fresh);
    assert(latch_result.modified && latch_result.synthetic_x != 0);
    latch_runtime.reportCompleted(0x84, true, latch_result, 51);
    latch_snapshot = latch_runtime.snapshot(51);
    assert(latch_snapshot.state == StickState::StickActive);
    assert(!latch_snapshot.physical_override && !latch_snapshot.awaiting_fresh);
    assert(latch_snapshot.counters.physical_override_reports == 2);
    assert(latch_snapshot.counters.physical_override_ms == 30);
    assert(latch_snapshot.counters.awaiting_fresh_ms == 10);
    assert(latch_runtime.handleLine("km.stick.status()", 51, response,
                                    sizeof(response)));
    assert(std::strstr(response, "awaiting=0/1/2/10") != nullptr);

    // A mode exit stops mutation immediately but ACKs only after one exact raw
    // controller report is delivered.
    assert(runtime.handleLine("km.output_mode(mouse)", 46, response,
                              sizeof(response)));
    assert(response[0] == '\0');
    result = runtime.compose(0x84, original, compose, sizeof(compose), 47);
    runtime.reportCompleted(0x84, true, result, 48);
    assert(runtime.takePendingResponse(response, sizeof(response)));
    assert(std::strstr(response, "STICK_ACK(output_mode") != nullptr);

    StickRuntime leave_timeout;
    (void)armStick(leave_timeout, 0xABCDEF02u, 1, profile);
    assert(leave_timeout.handleLine("km.output_mode(mouse)", 10, response,
                                    sizeof(response)));
    assert(response[0] == '\0');
    leave_timeout.checkStreamWatchdog(110);
    assert(leave_timeout.takePendingResponse(response, sizeof(response)));
    assert(std::strstr(response,
                       "STICK_NACK(output_mode,barrier_timeout,raw_safe)") != nullptr);
    timeout_snapshot = leave_timeout.snapshot(110);
    assert(timeout_snapshot.state == StickState::RawSafe);
    assert(timeout_snapshot.injection_session == 0);
    assert(timeout_snapshot.mode == StickOutputMode::Mouse);
    assert(leave_timeout.takeDetachRequest());
    assert(!leave_timeout.takeDetachRequest());

    leave_timeout.observeFifo(0x84, 8, 1, 3, false);
    assert(leave_timeout.handleLine("km.stick.status()", 111, response,
                                    sizeof(response)));
    assert(std::strstr(response, "barrier=1/0/0") != nullptr);
    assert(std::strstr(response, "target=1/8/1/") != nullptr);
    assert(std::strstr(response, "timing=0/0/0/0/0") != nullptr);

    // Every mode transition owns its barrier until completion or timeout.
    // Leave -> Begin must reject the new begin and preserve the Leave ACK.
    StickRuntime leave_begin_complete;
    (void)armStick(leave_begin_complete, 0xABCDEF03u, 1, profile);
    assert(leave_begin_complete.handleLine("km.output_mode(mouse)", 10,
                                           response, sizeof(response)));
    assert(response[0] == '\0');
    assert(leave_begin_complete.handleLine(
        "km.stick.begin(dse-capture-v1,none,state_q15)", 11, response,
        sizeof(response)));
    assert(std::strstr(response,
                       "STICK_NACK(begin,barrier_pending,") != nullptr);
    const uint8_t barrier_neutral[8] =
        {1, 0x11, 0x22, 128, 128, 0x55, 0x66, 0x77};
    uint8_t barrier_compose[8]{};
    std::memcpy(barrier_compose, barrier_neutral, sizeof(barrier_compose));
    result = leave_begin_complete.compose(
        0x84, barrier_neutral, barrier_compose, sizeof(barrier_compose), 12);
    leave_begin_complete.reportCompleted(0x84, true, result, 12);
    assert(leave_begin_complete.takePendingResponse(response, sizeof(response)));
    assert(std::strstr(response, "STICK_ACK(output_mode") != nullptr);
    assert(std::strstr(response, ",mouse,") != nullptr);
    assert(leave_begin_complete.snapshot(12).injection_session == 0);

    // The same rejected Leave -> Begin sequence must retain the original
    // timeout path when no fresh report arrives.
    StickRuntime leave_begin_timeout;
    (void)armStick(leave_begin_timeout, 0xABCDEF04u, 1, profile);
    assert(leave_begin_timeout.handleLine("km.output_mode(mouse)", 10,
                                          response, sizeof(response)));
    assert(leave_begin_timeout.handleLine(
        "km.stick.begin(dse-capture-v1,none,state_q15)", 11, response,
        sizeof(response)));
    assert(std::strstr(response,
                       "STICK_NACK(begin,barrier_pending,") != nullptr);
    leave_begin_timeout.checkStreamWatchdog(110);
    assert(leave_begin_timeout.takePendingResponse(response, sizeof(response)));
    assert(std::strstr(response,
                       "STICK_NACK(output_mode,barrier_timeout,") != nullptr);

    // Begin -> Leave must reject the leave and preserve the delayed Begin ACK.
    StickRuntime begin_leave_complete;
    begin_leave_complete.bindPhysical(0xABCDEF05u, 1);
    assert(begin_leave_complete.configureExactProfile(
        "dse-capture-v1", "none", profile, identity));
    assert(begin_leave_complete.handleLine(
        "km.stick.begin(dse-capture-v1,none,state_q15)", 10, response,
        sizeof(response)));
    assert(response[0] == '\0');
    assert(begin_leave_complete.handleLine("km.output_mode(mouse)", 11,
                                           response, sizeof(response)));
    assert(std::strstr(response,
                       "STICK_NACK(output_mode,barrier_pending,") != nullptr);
    for (uint64_t timestamp : {uint64_t(12), uint64_t(13)}) {
        std::memcpy(barrier_compose, barrier_neutral, sizeof(barrier_compose));
        result = begin_leave_complete.compose(
            0x84, barrier_neutral, barrier_compose, sizeof(barrier_compose),
            timestamp);
        begin_leave_complete.reportCompleted(0x84, true, result, timestamp);
    }
    assert(begin_leave_complete.takePendingResponse(response, sizeof(response)));
    assert(std::strstr(response, "STICK_ACK(begin") != nullptr);
    assert(begin_leave_complete.snapshot(13).injection_session != 0);

    // Begin -> Leave timeout is likewise owned by Begin after the rejection.
    StickRuntime begin_leave_timeout;
    begin_leave_timeout.bindPhysical(0xABCDEF06u, 1);
    assert(begin_leave_timeout.configureExactProfile(
        "dse-capture-v1", "none", profile, identity));
    assert(begin_leave_timeout.handleLine(
        "km.stick.begin(dse-capture-v1,none,state_q15)", 10, response,
        sizeof(response)));
    assert(begin_leave_timeout.handleLine("km.output_mode(mouse)", 11,
                                          response, sizeof(response)));
    assert(std::strstr(response,
                       "STICK_NACK(output_mode,barrier_pending,") != nullptr);
    begin_leave_timeout.checkStreamWatchdog(110);
    assert(begin_leave_timeout.takePendingResponse(response, sizeof(response)));
    assert(std::strstr(response,
                       "STICK_NACK(begin,barrier_timeout,") != nullptr);

    // Leave -> Leave cannot replace the first target mode or its ACK.
    StickRuntime leave_leave;
    (void)armStick(leave_leave, 0xABCDEF07u, 1, profile);
    assert(leave_leave.handleLine("km.output_mode(mouse)", 10,
                                  response, sizeof(response)));
    assert(leave_leave.handleLine("km.output_mode(off)", 11, response,
                                  sizeof(response)));
    assert(std::strstr(response,
                       "STICK_NACK(output_mode,barrier_pending,") != nullptr);
    std::memcpy(barrier_compose, barrier_neutral, sizeof(barrier_compose));
    result = leave_leave.compose(
        0x84, barrier_neutral, barrier_compose, sizeof(barrier_compose), 12);
    leave_leave.reportCompleted(0x84, true, result, 12);
    assert(leave_leave.takePendingResponse(response, sizeof(response)));
    assert(std::strstr(response, "STICK_ACK(output_mode") != nullptr);
    assert(std::strstr(response, ",mouse,") != nullptr);

    // A second begin while already in Stick mode must explicitly leave first.
    StickRuntime begin_while_stick;
    const uint32_t stick_injection =
        armStick(begin_while_stick, 0xABCDEF08u, 1, profile);
    assert(begin_while_stick.handleLine(
        "km.stick.begin(dse-capture-v1,none,state_q15)", 10, response,
        sizeof(response)));
    assert(std::strstr(response,
                       "STICK_NACK(begin,leave_stick_first,") != nullptr);
    auto stick_snapshot = begin_while_stick.snapshot(10);
    assert(stick_snapshot.mode == StickOutputMode::Stick);
    assert(stick_snapshot.injection_session == stick_injection);

    // Unknown/unconfigured profiles are fail-closed and never mutate bytes.
    StickRuntime raw_safe;
    raw_safe.bindPhysical(9, 1);
    assert(raw_safe.handleLine("km.stick.begin(guess,none,state_q15)", 2,
                               response, sizeof(response)));
    assert(std::strstr(response, "profile_mismatch") != nullptr);
    std::memcpy(compose, original, sizeof(compose));
    result = raw_safe.compose(0x84, original, compose, sizeof(compose), 3);
    assert(!result.qualifying_report && !result.modified);
    assert(std::memcmp(original, compose, sizeof(original)) == 0);
    return 0;
}
