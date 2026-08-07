#include "stick_runtime.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace makcm::native {
namespace {
constexpr float kPhase3MaxNorm = 0.20f;
constexpr uint64_t kBringupStreamWatchdogMs = 100;
constexpr uint64_t kControlBarrierTimeoutMs = 100;

bool validToken(const char *value) {
    if (value == nullptr) return false;
    const size_t length = std::strlen(value);
    if (length == 0 || length > 64) return false;
    for (size_t i = 0; i < length; ++i) {
        const char c = value[i];
        const bool valid = (c >= 'a' && c <= 'z') ||
                           (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || c == '_' || c == '-' ||
                           c == '.' || c == ':';
        if (!valid) return false;
    }
    return true;
}

int16_t boundedQ15(int32_t value) {
    return static_cast<int16_t>(std::clamp<int32_t>(value, -32767, 32767));
}

makcm::stick::StickQ15 clampPhase3(int32_t x, int32_t y, bool &saturated) {
    float nx = static_cast<float>(boundedQ15(x)) / 32767.0f;
    float ny = static_cast<float>(boundedQ15(y)) / 32767.0f;
    const float magnitude = std::hypot(nx, ny);
    saturated = magnitude > kPhase3MaxNorm;
    if (saturated && magnitude > 0.0f) {
        const float scale = kPhase3MaxNorm / magnitude;
        nx *= scale;
        ny *= scale;
    }
    return {makcm::stick::normalizedToQ15(nx),
            makcm::stick::normalizedToQ15(ny)};
}
} // namespace

StickRuntime::StickRuntime() { reset(); }

void StickRuntime::reset() {
    session_.reset();
    codec_ = {};
    override_.reset();
    mode_ = StickOutputMode::Mouse;
    pending_barrier_ = PendingBarrier::None;
    pending_barrier_started_ms_ = 0;
    arming_completed_ = 0;
    injection_counter_ = 0;
    last_report_ms_ = 0;
    detach_requested_ = false;
    stream_fault_ = false;
    expiry_center_emitted_ = false;
    last_override_ = false;
    override_started_ms_ = 0;
    last_awaiting_fresh_ = false;
    awaiting_fresh_started_ms_ = 0;
    last_emitted_sequence_ = 0;
    last_emitted_accepted_ms_ = 0;
    last_emitted_at_ms_ = 0;
    format_id_[0] = cal_id_[0] = pending_response_[0] = '\0';
    std::snprintf(control_profile_, sizeof(control_profile_), "hip");
    clearSynthetic();
    counters_ = {};
}

void StickRuntime::finishPhysicalOverride(uint64_t now_ms) {
    if (!last_override_) {
        override_started_ms_ = 0;
        return;
    }
    if (now_ms >= override_started_ms_) {
        counters_.physical_override_ms += static_cast<uint32_t>(
            std::min<uint64_t>(now_ms - override_started_ms_, UINT32_MAX));
    }
    last_override_ = false;
    override_started_ms_ = 0;
}

void StickRuntime::finishAwaitingFresh(uint64_t now_ms) {
    if (!last_awaiting_fresh_) {
        awaiting_fresh_started_ms_ = 0;
        return;
    }
    if (now_ms >= awaiting_fresh_started_ms_) {
        counters_.awaiting_fresh_ms += static_cast<uint32_t>(
            std::min<uint64_t>(now_ms - awaiting_fresh_started_ms_, UINT32_MAX));
    }
    last_awaiting_fresh_ = false;
    awaiting_fresh_started_ms_ = 0;
    last_emitted_sequence_ = 0;
    last_emitted_accepted_ms_ = 0;
    last_emitted_at_ms_ = 0;
}

void StickRuntime::bindPhysical(uint32_t physical_session, uint64_t now_ms) {
    finishPhysicalOverride(now_ms);
    finishAwaitingFresh(now_ms);
    session_.bindPhysical(physical_session, now_ms);
    mode_ = StickOutputMode::Mouse;
    pending_barrier_ = PendingBarrier::None;
    pending_barrier_started_ms_ = 0;
    arming_completed_ = 0;
    last_report_ms_ = 0;
    detach_requested_ = false;
    stream_fault_ = false;
    expiry_center_emitted_ = false;
    pending_response_[0] = '\0';
    std::snprintf(control_profile_, sizeof(control_profile_), "hip");
    override_.reset();
    last_override_ = false;
    override_started_ms_ = 0;
    last_awaiting_fresh_ = false;
    awaiting_fresh_started_ms_ = 0;
    last_emitted_sequence_ = 0;
    last_emitted_accepted_ms_ = 0;
    last_emitted_at_ms_ = 0;
    clearProfile();
    clearSynthetic();
}

void StickRuntime::hotplug(uint64_t now_ms) {
    finishPhysicalOverride(now_ms);
    finishAwaitingFresh(now_ms);
    session_.hotplug(now_ms);
    mode_ = StickOutputMode::Mouse;
    pending_barrier_ = PendingBarrier::None;
    pending_barrier_started_ms_ = 0;
    arming_completed_ = 0;
    last_report_ms_ = 0;
    detach_requested_ = false;
    stream_fault_ = false;
    expiry_center_emitted_ = false;
    pending_response_[0] = '\0';
    last_emitted_sequence_ = 0;
    last_emitted_accepted_ms_ = 0;
    last_emitted_at_ms_ = 0;
    std::snprintf(control_profile_, sizeof(control_profile_), "hip");
    override_.reset();
    last_override_ = false;
    override_started_ms_ = 0;
    last_awaiting_fresh_ = false;
    awaiting_fresh_started_ms_ = 0;
    clearProfile();
    clearSynthetic();
}

bool StickRuntime::configureExactProfile(
    const char *format_id, const char *cal_id,
    const makcm::stick::ControllerProfile &allowed,
    const makcm::stick::ControllerProfileIdentity &observed) {
    if (!validToken(format_id) || !validToken(cal_id) ||
        session_.physicalSession() == 0 || mode_ == StickOutputMode::Stick ||
        !codec_.configureExact(allowed, observed)) {
        ++counters_.profile_mismatch;
        return false;
    }
    std::snprintf(format_id_, sizeof(format_id_), "%s", format_id);
    std::snprintf(cal_id_, sizeof(cal_id_), "%s", cal_id);
    return true;
}

void StickRuntime::clearProfile() {
    codec_ = {};
    format_id_[0] = '\0';
    cal_id_[0] = '\0';
}

bool StickRuntime::handleLine(const char *line, uint64_t now_ms,
                              char *response, size_t response_capacity) {
    if (response != nullptr && response_capacity != 0) response[0] = '\0';
    if (line == nullptr) return false;
    if (std::strcmp(line, "km.caps()") == 0) {
        if (response != nullptr && response_capacity != 0) {
            std::snprintf(response, response_capacity,
                          "STICK_CAPS(v=2,state_q15=1,rate=0,move_compat=0,"
                          "game_cal=0,profiles=%u,max_ttl=80)\r\n",
                          codec_.configured() ? 1u : 0u);
        }
        return true;
    }
    if (std::strncmp(line, "km.stick.begin(", 15) == 0)
        return handleBegin(line, now_ms, response, response_capacity);
    if (std::strncmp(line, "km.output_mode(", 15) == 0)
        return handleOutputMode(line, now_ms, response, response_capacity);
    if (std::strcmp(line, "km.stick.status()") == 0) {
        writeStatus(now_ms, response, response_capacity);
        return true;
    }
    if (std::strncmp(line, "km.stick.profile(", 17) == 0) {
        char profile[4]{};
        char hash[65]{};
        char tail = '\0';
        const int fields = std::sscanf(
            line, "km.stick.profile(%3[^,],%64[^)])%c",
            profile, hash, &tail);
        if (fields != 2 ||
            (std::strcmp(profile, "hip") != 0 &&
             std::strcmp(profile, "ads") != 0) ||
            !validToken(hash)) {
            ++counters_.invalid_commands;
            ++counters_.frame_errors;
            writeNack("profile", "invalid_arguments", response,
                      response_capacity);
            return true;
        }
        if (!codec_.configured() || std::strcmp(hash, cal_id_) != 0) {
            ++counters_.invalid_commands;
            ++counters_.profile_mismatch;
            writeNack("profile", "calibration_mismatch", response,
                      response_capacity);
            return true;
        }
        std::snprintf(control_profile_, sizeof(control_profile_), "%s", profile);
        ++counters_.valid_commands;
        writeAck("profile", mode_, response, response_capacity);
        return true;
    }
    if (std::strcmp(line, "km.stick.clear_fault()") == 0) {
        if (stream_fault_) {
            finishPhysicalOverride(now_ms);
            finishAwaitingFresh(now_ms);
            stream_fault_ = false;
            detach_requested_ = false;
            session_.disarm();
            mode_ = StickOutputMode::Mouse;
            override_.reset();
        }
        writeAck("clear_fault", mode_, response, response_capacity);
        return true;
    }
    if (std::strncmp(line, "km.stick.state(", 15) == 0 ||
        std::strncmp(line, "km.stick.rate(", 14) == 0 ||
        std::strncmp(line, "km.stick.lease(", 15) == 0) {
        return handleData(line, now_ms, response, response_capacity);
    }
    if (std::strncmp(line, "km.stick.", 9) == 0) {
        ++counters_.invalid_commands;
        ++counters_.frame_errors;
        writeNack("unknown", "unsupported", response, response_capacity);
        return true;
    }
    return false;
}

bool StickRuntime::handleBegin(const char *line, uint64_t now_ms,
                               char *response, size_t response_capacity) {
    char format[65]{};
    char cal[65]{};
    char source[33]{};
    char tail = '\0';
    const int fields = std::sscanf(line, "km.stick.begin(%64[^,],%64[^,],%32[^)])%c",
                                   format, cal, source, &tail);
    if (fields != 3 || !validToken(format) || !validToken(cal) ||
        !validToken(source)) {
        ++counters_.invalid_commands;
        ++counters_.frame_errors;
        writeNack("begin", "invalid_arguments", response, response_capacity);
        return true;
    }
    if (session_.physicalSession() == 0) {
        ++counters_.invalid_commands;
        writeNack("begin", "no_physical_session", response, response_capacity);
        return true;
    }
    if (!codec_.configured() || std::strcmp(format, format_id_) != 0 ||
        std::strcmp(cal, cal_id_) != 0) {
        ++counters_.invalid_commands;
        ++counters_.profile_mismatch;
        writeNack("begin", "profile_mismatch", response, response_capacity);
        return true;
    }
    if (std::strcmp(source, "state_q15") != 0) {
        ++counters_.invalid_commands;
        writeNack("begin", "source_not_enabled", response, response_capacity);
        return true;
    }
    if (pending_barrier_ != PendingBarrier::None) {
        ++counters_.invalid_commands;
        writeNack("begin", "barrier_pending", response, response_capacity);
        return true;
    }
    if (mode_ == StickOutputMode::Stick) {
        ++counters_.invalid_commands;
        writeNack("begin", "leave_stick_first", response, response_capacity);
        return true;
    }
    ++injection_counter_;
    if (injection_counter_ == 0) ++injection_counter_;
    uint32_t injection = session_.physicalSession() ^
                         (injection_counter_ * 0x9E3779B9u) ^
                         static_cast<uint32_t>(now_ms);
    if (injection == 0) injection = injection_counter_;
    if (!session_.beginInjection(session_.physicalSession(), injection,
                                 makcm::stick::StickSource::StateQ15, now_ms)) {
        ++counters_.invalid_commands;
        writeNack("begin", "session_reject", response, response_capacity);
        return true;
    }
    mode_ = StickOutputMode::Mouse;
    pending_barrier_ = PendingBarrier::Begin;
    pending_barrier_started_ms_ = now_ms;
    arming_completed_ = 0;
    pending_response_[0] = '\0';
    finishPhysicalOverride(now_ms);
    finishAwaitingFresh(now_ms);
    override_.reset();
    last_override_ = false;
    override_started_ms_ = 0;
    last_awaiting_fresh_ = false;
    awaiting_fresh_started_ms_ = 0;
    clearSynthetic();
    ++counters_.valid_commands;
    // ACK is deliberately delayed until two exact reports complete unchanged.
    return true;
}

bool StickRuntime::handleOutputMode(const char *line, uint64_t now_ms,
                                    char *response, size_t response_capacity) {
    char requested[17]{};
    char tail = '\0';
    const int fields = std::sscanf(line, "km.output_mode(%16[^)])%c", requested, &tail);
    if (fields != 1) {
        ++counters_.invalid_commands;
        ++counters_.frame_errors;
        writeNack("output_mode", "invalid_arguments", response, response_capacity);
        return true;
    }
    if (std::strcmp(requested, "stick") == 0) {
        if (pending_barrier_ != PendingBarrier::None) {
            ++counters_.invalid_commands;
            writeNack("output_mode", "barrier_pending", response,
                      response_capacity);
            return true;
        }
        if (!codec_.configured() || session_.injectionSession() == 0 ||
            session_.state() != makcm::stick::StickState::StickIdle ||
            pending_barrier_ != PendingBarrier::None) {
            ++counters_.invalid_commands;
            writeNack("output_mode", "not_armed", response, response_capacity);
            return true;
        }
        if (mode_ != StickOutputMode::Stick) ++counters_.mode_switches;
        mode_ = StickOutputMode::Stick;
        ++counters_.valid_commands;
        writeAck("output_mode", mode_, response, response_capacity);
        return true;
    }
    if (std::strcmp(requested, "mouse") != 0 &&
        std::strcmp(requested, "off") != 0) {
        ++counters_.invalid_commands;
        writeNack("output_mode", "unsupported_mode", response, response_capacity);
        return true;
    }
    const StickOutputMode target = std::strcmp(requested, "off") == 0
                                       ? StickOutputMode::Off
                                       : StickOutputMode::Mouse;
    if (pending_barrier_ != PendingBarrier::None) {
        ++counters_.invalid_commands;
        writeNack("output_mode", "barrier_pending", response,
                  response_capacity);
        return true;
    }
    if (session_.injectionSession() == 0) {
        if (mode_ != target) ++counters_.mode_switches;
        mode_ = target;
        pending_barrier_ = PendingBarrier::None;
        pending_barrier_started_ms_ = 0;
        arming_completed_ = 0;
        finishPhysicalOverride(now_ms);
        finishAwaitingFresh(now_ms);
        override_.reset();
        clearSynthetic();
        writeAck("output_mode", target, response, response_capacity);
        return true;
    }
    if (mode_ != target) ++counters_.mode_switches;
    mode_ = target; // stop mutation immediately; ACK waits for one raw report.
    // Leaving stick output terminates both physical-priority intervals at the
    // mode transition.  The raw-report barrier below is only for the ACK and
    // must not keep stale override/awaiting state visible while waiting.
    finishPhysicalOverride(now_ms);
    finishAwaitingFresh(now_ms);
    override_.reset();
    clearSynthetic();
    pending_barrier_ = target == StickOutputMode::Off
                           ? PendingBarrier::LeaveOff
                           : PendingBarrier::LeaveMouse;
    pending_barrier_started_ms_ = now_ms;
    pending_response_[0] = '\0';
    ++counters_.valid_commands;
    return true;
}

bool StickRuntime::handleData(const char *line, uint64_t now_ms,
                              char *response, size_t response_capacity) {
    makcm::stick::StickCommand command;
    makcm::stick::StickParseError parse_error = makcm::stick::StickParseError::None;
    if (!makcm::stick::parseStickCommand(line, std::strlen(line), command,
                                         &parse_error)) {
        ++counters_.invalid_commands;
        if (parse_error == makcm::stick::StickParseError::CrcMismatch ||
            parse_error == makcm::stick::StickParseError::InvalidCrcHex ||
            parse_error == makcm::stick::StickParseError::MissingCrc)
            ++counters_.crc_errors;
        else
            ++counters_.frame_errors;
        // Data plane normally has no ACK; malformed data is observable via
        // counters/status and cannot refresh seq or TTL.
        (void)response;
        (void)response_capacity;
        return true;
    }
    if (command.source != makcm::stick::StickSource::StateQ15 ||
        mode_ != StickOutputMode::Stick) {
        ++counters_.invalid_commands;
        return true;
    }
    command.physicalSession = session_.physicalSession();
    const auto accepted = session_.accept(command, session_.physicalSession(), now_ms);
    if (!accepted.accepted) {
        ++counters_.invalid_commands;
        return true;
    }
    if (accepted.sequence.disposition ==
        makcm::stick::SequenceDisposition::NewWithGap) {
        // The exact gap is retained in SessionCounters.
    }
    const bool awaiting_before = last_awaiting_fresh_;
    override_.notifyFreshSequence(command.sequence);
    if (awaiting_before && !override_.awaitingFresh())
        finishAwaitingFresh(now_ms);
    const auto reversed = [](int32_t previous, int32_t next) {
        return previous != 0 && next != 0 &&
               ((previous < 0 && next > 0) || (previous > 0 && next < 0));
    };
    if (reversed(last_command_x_, command.x)) {
        reversal_pending_x_ = true;
        ++reversal_epoch_x_;
        if (reversal_epoch_x_ == 0) ++reversal_epoch_x_;
    }
    if (reversed(last_command_y_, command.y)) {
        reversal_pending_y_ = true;
        ++reversal_epoch_y_;
        if (reversal_epoch_y_ == 0) ++reversal_epoch_y_;
    }
    last_command_x_ = command.x;
    last_command_y_ = command.y;
    expiry_center_emitted_ = false;
    ++counters_.valid_commands;
    return true;
}

StickComposeResult StickRuntime::compose(uint8_t endpoint_address,
                                         const uint8_t *original,
                                         uint8_t *compose_buffer,
                                         uint16_t length, uint64_t now_ms) {
    StickComposeResult result;
    if (original == nullptr || compose_buffer == nullptr) return result;
    if (!codec_.configured() ||
        endpoint_address != codec_.profile().identity.endpointAddress ||
        length != codec_.profile().identity.reportLength ||
        (codec_.profile().identity.reportId != 0 &&
         original[0] != codec_.profile().identity.reportId)) {
        return result;
    }
    result.qualifying_report = true;
    last_report_ms_ = now_ms;

    if (mode_ != StickOutputMode::Stick ||
        session_.state() == makcm::stick::StickState::StickArming ||
        session_.state() == makcm::stick::StickState::RawSafe ||
        session_.state() == makcm::stick::StickState::Hotplug ||
        session_.state() == makcm::stick::StickState::Fault) {
        return result;
    }

    makcm::stick::StickQ15 physical_q15{};
    if (!codec_.decodePhysical(original, length, physical_q15, &result.codec_error)) {
        ++counters_.codec_decode_error;
        return result;
    }
    result.physical_x = physical_q15.x;
    result.physical_y = physical_q15.y;
    const makcm::stick::StickVector physical{
        makcm::stick::q15ToNormalized(physical_q15.x),
        makcm::stick::q15ToNormalized(physical_q15.y)};
    const bool was_latched = override_.latched();
    const bool was_awaiting_fresh = override_.awaitingFresh();
    const bool latched = override_.observe(
        physical, session_.hasSequence() ? session_.lastSequence() : 0, now_ms);
    const bool awaiting_fresh = override_.awaitingFresh();

    // A released latch ends the active-override interval immediately.  The
    // fresh-sequence barrier is tracked separately and is intentionally not
    // counted as active physical override time.
    if (was_latched && !latched) {
        finishPhysicalOverride(now_ms);
        // A released latch always enters the fresh-sequence barrier from an
        // idle session.  A later accepted sequence is responsible for
        // restoring StickActive; it must not be overwritten by old latch
        // bookkeeping.
        session_.setState(makcm::stick::StickState::StickIdle);
    }
    if (awaiting_fresh && !last_awaiting_fresh_) {
        last_awaiting_fresh_ = true;
        awaiting_fresh_started_ms_ = now_ms;
        ++counters_.awaiting_fresh_count;
    } else if (!awaiting_fresh && was_awaiting_fresh &&
               last_awaiting_fresh_) {
        finishAwaitingFresh(now_ms);
    }

    if (latched) {
        if (!last_override_) {
            ++counters_.physical_override_count;
            override_started_ms_ = now_ms;
        }
        last_override_ = true;
        result.physical_override = true;
        result.merged_x = physical_q15.x;
        result.merged_y = physical_q15.y;
        session_.setState(makcm::stick::StickState::PhysicalOverride);
        return result;
    }
    if (awaiting_fresh) {
        // Keep the physical report byte-for-byte intact while the post-latch
        // fresh-sequence barrier is active.  Do not encode logical zero into
        // the controller report.
        result.awaiting_fresh = true;
        result.merged_x = physical_q15.x;
        result.merged_y = physical_q15.y;
        session_.setState(makcm::stick::StickState::StickIdle);
        return result;
    }

    const auto sample = session_.sample(now_ms);
    if (sample.centered || sample.expired) {
        // A logical synthetic center is a fail-close state, not authority to
        // rewrite real sub-threshold controller jitter to an encoded center.
        result.forced_center = sample.expired && !expiry_center_emitted_;
        result.merged_x = physical_q15.x;
        result.merged_y = physical_q15.y;
        if (session_.hasSequence()) {
            result.command_emitted = true;
            result.command_sequence = session_.lastSequence();
            result.command_accepted_ms = session_.acceptedAtMs();
        }
        return result;
    }

    int32_t synthetic_x = sample.x;
    int32_t synthetic_y = sample.y;
    if (reversal_pending_x_) synthetic_x = 0;
    if (reversal_pending_y_) synthetic_y = 0;
    const uint32_t reversal_epoch_x =
        reversal_pending_x_ ? reversal_epoch_x_ : 0;
    const uint32_t reversal_epoch_y =
        reversal_pending_y_ ? reversal_epoch_y_ : 0;

    bool saturated = false;
    const auto synthetic = clampPhase3(synthetic_x, synthetic_y, saturated);
    const makcm::stick::StickVector synthetic_normalized{
        makcm::stick::q15ToNormalized(synthetic.x),
        makcm::stick::q15ToNormalized(synthetic.y)};
    const auto merged_normalized =
        override_.merge(physical, synthetic_normalized);
    // PhysicalPriorityOverride::merge() is the source of truth for the
    // neutral-only hysteresis band.  Its physical result is byte-preserved;
    // only a synthetic result is allowed to pass through composeCopy().
    const bool physical_selected =
        !override_.latched() && !override_.awaitingFresh() &&
        makcm::stick::stickMagnitude(physical) >=
            override_.config().overrideOff &&
        merged_normalized.x == physical.x &&
        merged_normalized.y == physical.y;
    if (physical_selected) {
        // Neutral-only merge policy: preserve the original FIFO report when
        // physical input is authoritative, including the off/on hysteresis
        // band before a latch is established.
        result.merged_x = physical_q15.x;
        result.merged_y = physical_q15.y;
        return result;
    }

    const makcm::stick::StickQ15 merged{
        makcm::stick::normalizedToQ15(merged_normalized.x),
        makcm::stick::normalizedToQ15(merged_normalized.y)};
    result.saturated = saturated;
    result.synthetic_x = merged.x;
    result.synthetic_y = merged.y;
    result.reversal_epoch_x = reversal_epoch_x;
    result.reversal_epoch_y = reversal_epoch_y;
    makcm::stick::MutationResult mutation;
    if (!codec_.composeCopy(original, compose_buffer, length,
                            merged_normalized, &mutation)) {
        result.codec_error = mutation.error;
        if (mutation.error == makcm::stick::CodecError::MutationGuard)
            ++counters_.mutation_guard_error;
        return result;
    }
    result.codec_error = makcm::stick::CodecError::None;
    result.modified = mutation.changed;
    result.merged_x = merged.x;
    result.merged_y = merged.y;
    result.command_emitted = true;
    result.command_sequence = session_.lastSequence();
    result.command_accepted_ms = session_.acceptedAtMs();
    return result;
}

void StickRuntime::observeFifo(uint8_t endpoint_address,
                               uint16_t report_length, uint8_t report_id,
                               uint8_t depth, bool dropped) {
    if (!codec_.configured() ||
        endpoint_address != codec_.profile().identity.endpointAddress) return;
    counters_.controller_last_report_length = report_length;
    // report_id==0 means the profile has no report-ID prefix.  Keep this
    // telemetry explicitly named as the raw first byte so it cannot be
    // mistaken for a protocol report ID in that common XInput case.
    counters_.controller_last_first_byte = report_id;
    if (!dropped) ++counters_.controller_reports_received;
    counters_.controller_fifo_high_water = std::max(
        counters_.controller_fifo_high_water, depth);
    if (dropped) ++counters_.controller_fifo_drop;
}

void StickRuntime::reportCompleted(uint8_t endpoint_address, bool delivered,
                                   const StickComposeResult &compose_result,
                                   uint64_t now_ms) {
    if (!compose_result.qualifying_report || !delivered || !codec_.configured() ||
        endpoint_address != codec_.profile().identity.endpointAddress) return;
    ++counters_.reports_seen;
    if (compose_result.modified) ++counters_.reports_modified;
    if (compose_result.forced_center) {
        ++counters_.reports_forced_center;
        expiry_center_emitted_ = true;
    }
    if (compose_result.physical_override)
        ++counters_.physical_override_reports;
    if (compose_result.awaiting_fresh)
        ++counters_.awaiting_fresh_reports;
    if (compose_result.saturated) ++counters_.saturation;
    bool reversal_centered = false;
    if (compose_result.reversal_epoch_x != 0 && reversal_pending_x_ &&
        compose_result.reversal_epoch_x == reversal_epoch_x_) {
        reversal_pending_x_ = false;
        reversal_centered = true;
    }
    if (compose_result.reversal_epoch_y != 0 && reversal_pending_y_ &&
        compose_result.reversal_epoch_y == reversal_epoch_y_) {
        reversal_pending_y_ = false;
        reversal_centered = true;
    }
    if (reversal_centered) ++counters_.direction_reversal_centers;
    last_synthetic_x_ = compose_result.synthetic_x;
    last_synthetic_y_ = compose_result.synthetic_y;
    last_physical_x_ = compose_result.physical_x;
    last_physical_y_ = compose_result.physical_y;
    last_merged_x_ = compose_result.merged_x;
    last_merged_y_ = compose_result.merged_y;
    if (compose_result.command_emitted) {
        last_emitted_sequence_ = compose_result.command_sequence;
        last_emitted_accepted_ms_ = compose_result.command_accepted_ms;
        last_emitted_at_ms_ = now_ms;
    }
    if (pending_barrier_ == PendingBarrier::Begin &&
        session_.state() == makcm::stick::StickState::StickArming) {
        ++arming_completed_;
        ++counters_.arming_completions;
        if (arming_completed_ >= 2) {
            session_.setState(makcm::stick::StickState::StickIdle);
            pending_barrier_ = PendingBarrier::None;
            pending_barrier_started_ms_ = 0;
            queueAck("begin", StickOutputMode::Mouse);
        }
        return;
    }
    if (pending_barrier_ == PendingBarrier::LeaveMouse ||
        pending_barrier_ == PendingBarrier::LeaveOff) {
        const StickOutputMode completed_mode =
            pending_barrier_ == PendingBarrier::LeaveOff
                ? StickOutputMode::Off
                : StickOutputMode::Mouse;
        pending_barrier_ = PendingBarrier::None;
        pending_barrier_started_ms_ = 0;
        finishPhysicalOverride(now_ms);
        finishAwaitingFresh(now_ms);
        session_.disarm();
        override_.reset();
        queueAck("output_mode", completed_mode);
    }
}

void StickRuntime::checkStreamWatchdog(uint64_t now_ms) {
    checkControlBarrierTimeout(now_ms);
    const auto state = session_.state();
    const bool non_neutral_hold =
        state == makcm::stick::StickState::StickActive ||
        state == makcm::stick::StickState::PhysicalOverride ||
        state == makcm::stick::StickState::Expired;
    if (mode_ != StickOutputMode::Stick || !non_neutral_hold ||
        last_report_ms_ == 0 || now_ms < last_report_ms_ ||
        now_ms - last_report_ms_ <= kBringupStreamWatchdogMs) return;
    ++counters_.stream_stalls;
    stream_fault_ = true;
    detach_requested_ = true;
    mode_ = StickOutputMode::Off;
    finishPhysicalOverride(now_ms);
    finishAwaitingFresh(now_ms);
    override_.reset();
    session_.setState(makcm::stick::StickState::Fault);
    clearSynthetic();
}

void StickRuntime::checkControlBarrierTimeout(uint64_t now_ms) {
    if (pending_barrier_ == PendingBarrier::None ||
        now_ms < pending_barrier_started_ms_ ||
        now_ms - pending_barrier_started_ms_ < kControlBarrierTimeoutMs) {
        return;
    }

    const PendingBarrier timed_out = pending_barrier_;
    pending_barrier_ = PendingBarrier::None;
    pending_barrier_started_ms_ = 0;
    arming_completed_ = 0;
    ++counters_.barrier_timeouts;

    // The output path was already Mouse/Off for every delayed barrier.  The
    // timeout additionally revokes the injection lease so a missing or
    // mismatched controller report can never leave StickArming resident.
    if (timed_out == PendingBarrier::Begin) mode_ = StickOutputMode::Mouse;
    finishPhysicalOverride(now_ms);
    finishAwaitingFresh(now_ms);
    override_.reset();
    clearSynthetic();
    session_.disarm();
    // A Leave barrier may have been entered while one modified controller
    // report was already owned by TinyUSB.  Disarming prevents all future
    // mutation, but only a downstream detach can cancel that stale inflight
    // transfer and uphold the no-old-state barrier if it never completes.
    if (timed_out == PendingBarrier::LeaveMouse ||
        timed_out == PendingBarrier::LeaveOff) {
        detach_requested_ = true;
    }
    queueNack(timed_out == PendingBarrier::Begin ? "begin" : "output_mode",
              "barrier_timeout");
}

bool StickRuntime::takeDetachRequest() {
    const bool requested = detach_requested_;
    detach_requested_ = false;
    return requested;
}

void StickRuntime::writeAck(const char *op, StickOutputMode mode,
                            char *response, size_t response_capacity) const {
    if (response == nullptr || response_capacity == 0) return;
    std::snprintf(response, response_capacity,
                  "STICK_ACK(%s,%lu,%lu,%s,%s,%s)\r\n", op,
                  static_cast<unsigned long>(session_.physicalSession()),
                  static_cast<unsigned long>(session_.injectionSession()),
                  modeName(mode), format_id_[0] ? format_id_ : "none",
                  cal_id_[0] ? cal_id_ : "none");
}

void StickRuntime::writeNack(const char *op, const char *reason,
                             char *response, size_t response_capacity) const {
    if (response == nullptr || response_capacity == 0) return;
    std::snprintf(response, response_capacity, "STICK_NACK(%s,%s,%s)\r\n",
                  op, reason, stateName(session_.state()));
}

void StickRuntime::queueAck(const char *op, StickOutputMode mode) {
    writeAck(op, mode, pending_response_, sizeof(pending_response_));
}

void StickRuntime::queueNack(const char *op, const char *reason) {
    writeNack(op, reason, pending_response_, sizeof(pending_response_));
}

bool StickRuntime::takePendingResponse(char *response,
                                       size_t response_capacity) {
    if (response == nullptr || response_capacity == 0 ||
        pending_response_[0] == '\0') return false;
    std::snprintf(response, response_capacity, "%s", pending_response_);
    pending_response_[0] = '\0';
    return true;
}

void StickRuntime::writeStatus(uint64_t now_ms, char *response,
                               size_t response_capacity) {
    if (response == nullptr || response_capacity == 0) return;
    const auto snap = snapshot(now_ms);
    const uint32_t barrier_age = pending_barrier_ == PendingBarrier::None ||
                                         now_ms < pending_barrier_started_ms_
                                     ? 0
                                     : static_cast<uint32_t>(std::min<uint64_t>(
                                           now_ms - pending_barrier_started_ms_,
                                           UINT32_MAX));
    const int written = std::snprintf(
        response, response_capacity,
        "STICK_STATUS(%lu,%lu,%s,%s,%lu,%u,%s,%s,%d,%d,%d,%d,%d,%d,%u,%u,"
        "cmd=%lu/%lu,crc=%lu,seq=%lu/%lu/%lu,ttl=%lu,profile=%s/%lu,modified=%lu,"
        "center=%lu,override=%lu/%lu/%lu,sat=%lu,guard=%lu,decode=%lu,stall=%lu,"
        "fifo=%u/%lu,mode_switch=%lu,reversal=%lu,wrong=%lu/%lu,"
        "source_conflict=%lu,frame=%lu,barrier=%lu/%u/%lu,"
        "target=%lu/%u/%u/%lu,timing=%lu/%llu/%lu/%llu/%llu,"
        "awaiting=%u/%lu/%lu/%lu)\r\n",
        static_cast<unsigned long>(snap.physical_session),
        static_cast<unsigned long>(snap.injection_session), stateName(snap.state),
        sourceName(snap.source), static_cast<unsigned long>(snap.last_sequence),
        static_cast<unsigned>(snap.command_age_ms),
        format_id_[0] ? format_id_ : "none", cal_id_[0] ? cal_id_ : "none",
        snap.synthetic_x, snap.synthetic_y, snap.physical_x, snap.physical_y,
        snap.merged_x, snap.merged_y, snap.physical_override ? 1u : 0u,
        snap.stream_fault ? 1u : 0u,
        static_cast<unsigned long>(counters_.valid_commands),
        static_cast<unsigned long>(counters_.invalid_commands),
        static_cast<unsigned long>(counters_.crc_errors),
        static_cast<unsigned long>(session_.counters().sequenceGap),
        static_cast<unsigned long>(session_.counters().staleSequence),
        static_cast<unsigned long>(session_.counters().duplicateSequence),
        static_cast<unsigned long>(session_.counters().ttlExpiry),
        control_profile_,
        static_cast<unsigned long>(counters_.profile_mismatch),
        static_cast<unsigned long>(counters_.reports_modified),
        static_cast<unsigned long>(counters_.reports_forced_center),
        static_cast<unsigned long>(counters_.physical_override_count),
        static_cast<unsigned long>(counters_.physical_override_reports),
        static_cast<unsigned long>(snap.counters.physical_override_ms),
        static_cast<unsigned long>(counters_.saturation),
        static_cast<unsigned long>(counters_.mutation_guard_error),
        static_cast<unsigned long>(counters_.codec_decode_error),
        static_cast<unsigned long>(counters_.stream_stalls),
        static_cast<unsigned>(counters_.controller_fifo_high_water),
        static_cast<unsigned long>(counters_.controller_fifo_drop),
        static_cast<unsigned long>(counters_.mode_switches),
        static_cast<unsigned long>(counters_.direction_reversal_centers),
        static_cast<unsigned long>(session_.counters().wrongPhysicalSession),
        static_cast<unsigned long>(session_.counters().wrongInjectionSession),
        static_cast<unsigned long>(session_.counters().sourceConflict),
        static_cast<unsigned long>(counters_.frame_errors),
        static_cast<unsigned long>(counters_.barrier_timeouts),
        static_cast<unsigned>(pending_barrier_),
        static_cast<unsigned long>(barrier_age),
        static_cast<unsigned long>(counters_.controller_reports_received),
        static_cast<unsigned>(counters_.controller_last_report_length),
        static_cast<unsigned>(counters_.controller_last_first_byte),
        static_cast<unsigned long>(counters_.reports_seen),
        static_cast<unsigned long>(session_.hasSequence()
                                       ? session_.lastSequence()
                                       : 0),
        static_cast<unsigned long long>(session_.hasSequence()
                                            ? session_.acceptedAtMs()
                                            : 0),
        static_cast<unsigned long>(last_emitted_sequence_),
        static_cast<unsigned long long>(last_emitted_accepted_ms_),
        static_cast<unsigned long long>(last_emitted_at_ms_),
        snap.awaiting_fresh ? 1u : 0u,
        static_cast<unsigned long>(snap.counters.awaiting_fresh_count),
        static_cast<unsigned long>(snap.counters.awaiting_fresh_reports),
        static_cast<unsigned long>(snap.counters.awaiting_fresh_ms));
    if (written < 0 || static_cast<size_t>(written) >= response_capacity) {
        writeNack("status", "truncated", response, response_capacity);
    }
}

StickRuntimeSnapshot StickRuntime::snapshot(uint64_t now_ms) {
    StickRuntimeSnapshot result;
    result.mode = mode_;
    result.source = session_.source();
    result.physical_session = session_.physicalSession();
    result.injection_session = session_.injectionSession();
    result.last_sequence = session_.lastSequence();
    bool logically_expired = false;
    if (session_.hasSequence()) {
        if (last_awaiting_fresh_) {
            // The pre-override command is deliberately ineligible while the
            // fresh-sequence barrier is active.  Reporting its age must not
            // mutate StickIdle into Expired or count an obsolete TTL lease.
            const uint64_t accepted_ms = session_.acceptedAtMs();
            const uint64_t age = now_ms >= accepted_ms ? now_ms - accepted_ms : 0;
            result.command_age_ms = static_cast<uint16_t>(
                std::min<uint64_t>(age, UINT16_MAX));
        } else {
            const auto observed = session_.peek(now_ms);
            result.command_age_ms = observed.ageMs;
            logically_expired = observed.expired;
        }
    }
    // A snapshot is a pure observation.  Report a derived Expired state when
    // peek() sees the deadline, but leave session_ and ttlExpiry untouched;
    // compose()/watchdog remain the only paths that advance TTL state.
    result.state = logically_expired ? makcm::stick::StickState::Expired
                                     : session_.state();
    result.synthetic_x = last_synthetic_x_;
    result.synthetic_y = last_synthetic_y_;
    result.physical_x = last_physical_x_;
    result.physical_y = last_physical_y_;
    result.merged_x = last_merged_x_;
    result.merged_y = last_merged_y_;
    result.profile_ready = codec_.configured();
    // Active-latch and post-latch awaiting state are intentionally separate
    // telemetry dimensions.  In particular, awaitingFresh is not an active
    // physical override and must not inflate its counters or duration.
    result.physical_override = last_override_;
    result.awaiting_fresh = last_awaiting_fresh_;
    result.stream_fault = stream_fault_;
    result.counters = counters_;
    if (last_override_ && now_ms >= override_started_ms_) {
        result.counters.physical_override_ms += static_cast<uint32_t>(
            std::min<uint64_t>(now_ms - override_started_ms_, UINT32_MAX));
    }
    if (last_awaiting_fresh_ && now_ms >= awaiting_fresh_started_ms_) {
        result.counters.awaiting_fresh_ms += static_cast<uint32_t>(
            std::min<uint64_t>(now_ms - awaiting_fresh_started_ms_, UINT32_MAX));
    }
    result.session_counters = session_.counters();
    return result;
}

void StickRuntime::clearSynthetic() {
    last_synthetic_x_ = last_synthetic_y_ = 0;
    last_merged_x_ = last_merged_y_ = 0;
    last_command_x_ = last_command_y_ = 0;
    reversal_pending_x_ = reversal_pending_y_ = false;
    reversal_epoch_x_ = reversal_epoch_y_ = 0;
}

const char *StickRuntime::modeName(StickOutputMode mode) {
    switch (mode) {
    case StickOutputMode::Off: return "off";
    case StickOutputMode::Mouse: return "mouse";
    case StickOutputMode::Stick: return "stick";
    }
    return "off";
}

const char *StickRuntime::sourceName(makcm::stick::StickSource source) {
    switch (source) {
    case makcm::stick::StickSource::StateQ15: return "state_q15";
    case makcm::stick::StickSource::Rate: return "rate";
    case makcm::stick::StickSource::MoveCompat: return "move_compat";
    case makcm::stick::StickSource::None: return "none";
    }
    return "none";
}

const char *StickRuntime::stateName(makcm::stick::StickState state) {
    switch (state) {
    case makcm::stick::StickState::RawSafe: return "raw_safe";
    case makcm::stick::StickState::StickArming: return "stick_arming";
    case makcm::stick::StickState::StickIdle: return "stick_idle";
    case makcm::stick::StickState::StickActive: return "stick_active";
    case makcm::stick::StickState::PhysicalOverride: return "physical_override";
    case makcm::stick::StickState::Expired: return "expired";
    case makcm::stick::StickState::Fault: return "fault";
    case makcm::stick::StickState::Hotplug: return "hotplug";
    }
    return "fault";
}

} // namespace makcm::native
