#pragma once

#include <stddef.h>
#include <stdint.h>

#include "controller_codec.h"
#include "stick_protocol.h"
#include "stick_session.h"
#include "stick_synth.h"

namespace makcm::native {

enum class StickOutputMode : uint8_t { Off = 0, Mouse = 1, Stick = 2 };

// The complete status frame is intentionally larger than the short control
// replies.  Callers may pass a smaller buffer, in which case writeStatus()
// returns an explicit truncated NACK instead of a partial status line.
inline constexpr size_t kStickStatusCapacity = 1024;

struct StickRuntimeCounters {
    uint32_t valid_commands = 0;
    uint32_t invalid_commands = 0;
    uint32_t crc_errors = 0;
    uint32_t frame_errors = 0;
    uint32_t profile_mismatch = 0;
    uint32_t codec_decode_error = 0;
    uint32_t reports_seen = 0;
    uint32_t reports_modified = 0;
    uint32_t reports_forced_center = 0;
    uint32_t mutation_guard_error = 0;
    uint32_t physical_override_count = 0;
    uint32_t physical_override_reports = 0;
    uint32_t physical_override_ms = 0;
    uint32_t awaiting_fresh_count = 0;
    uint32_t awaiting_fresh_reports = 0;
    uint32_t awaiting_fresh_ms = 0;
    uint32_t saturation = 0;
    uint32_t mode_switches = 0;
    uint32_t stream_stalls = 0;
    uint32_t barrier_timeouts = 0;
    uint32_t arming_completions = 0;
    uint32_t controller_reports_received = 0;
    uint16_t controller_last_report_length = 0;
    uint8_t controller_last_first_byte = 0;
    uint32_t controller_fifo_drop = 0;
    uint8_t controller_fifo_high_water = 0;
    uint32_t direction_reversal_centers = 0;
};

struct StickRuntimeSnapshot {
    StickOutputMode mode = StickOutputMode::Mouse;
    makcm::stick::StickState state = makcm::stick::StickState::RawSafe;
    makcm::stick::StickSource source = makcm::stick::StickSource::None;
    uint32_t physical_session = 0;
    uint32_t injection_session = 0;
    uint32_t last_sequence = 0;
    uint16_t command_age_ms = 0;
    int16_t synthetic_x = 0;
    int16_t synthetic_y = 0;
    int16_t physical_x = 0;
    int16_t physical_y = 0;
    int16_t merged_x = 0;
    int16_t merged_y = 0;
    bool profile_ready = false;
    bool physical_override = false;
    bool awaiting_fresh = false;
    bool stream_fault = false;
    StickRuntimeCounters counters{};
    makcm::stick::SessionCounters session_counters{};
};

struct StickComposeResult {
    bool qualifying_report = false;
    bool modified = false;
    bool forced_center = false;
    bool physical_override = false;
    bool awaiting_fresh = false;
    bool saturated = false;
    bool command_emitted = false;
    uint32_t command_sequence = 0;
    uint64_t command_accepted_ms = 0;
    int16_t synthetic_x = 0;
    int16_t synthetic_y = 0;
    int16_t physical_x = 0;
    int16_t physical_y = 0;
    int16_t merged_x = 0;
    int16_t merged_y = 0;
    uint32_t reversal_epoch_x = 0;
    uint32_t reversal_epoch_y = 0;
    makcm::stick::CodecError codec_error =
        makcm::stick::CodecError::ProfileNotConfigured;
};

// The caller serializes all methods with EndpointRouter's existing mux.  This
// class owns no endpoint storage and can never advance, replace, or retire a
// FIFO slot.  It only receives a read-only FIFO-head image and a compose copy.
class StickRuntime final {
public:
    StickRuntime();

    void reset();
    void bindPhysical(uint32_t physical_session, uint64_t now_ms);
    void hotplug(uint64_t now_ms);

    // Exact profiles are installed only from verified capture data.  No
    // VID/PID wildcard or guessed report offset is accepted by this API.
    bool configureExactProfile(const char *format_id, const char *cal_id,
                               const makcm::stick::ControllerProfile &allowed,
                               const makcm::stick::ControllerProfileIdentity &observed);
    void clearProfile();

    // Returns true when the line belongs to the stick protocol. A synchronous
    // reply, if any, is written to response. Delayed barrier ACKs are obtained
    // through takePendingResponse().
    bool handleLine(const char *line, uint64_t now_ms,
                    char *response, size_t response_capacity);
    bool takePendingResponse(char *response, size_t response_capacity);

    StickComposeResult compose(uint8_t endpoint_address,
                               const uint8_t *original, uint8_t *compose,
                               uint16_t length, uint64_t now_ms);
    void reportCompleted(uint8_t endpoint_address, bool delivered,
                         const StickComposeResult &compose_result,
                         uint64_t now_ms);
    void observeFifo(uint8_t endpoint_address, uint16_t report_length,
                     uint8_t report_id, uint8_t depth, bool dropped);

    void checkStreamWatchdog(uint64_t now_ms);
    bool takeDetachRequest();
    StickRuntimeSnapshot snapshot(uint64_t now_ms);

private:
    enum class PendingBarrier : uint8_t { None, Begin, LeaveMouse, LeaveOff };

    bool handleBegin(const char *line, uint64_t now_ms,
                     char *response, size_t response_capacity);
    bool handleOutputMode(const char *line, uint64_t now_ms,
                          char *response, size_t response_capacity);
    bool handleData(const char *line, uint64_t now_ms,
                    char *response, size_t response_capacity);
    void writeAck(const char *op, StickOutputMode mode,
                  char *response, size_t response_capacity) const;
    void writeNack(const char *op, const char *reason,
                   char *response, size_t response_capacity) const;
    void writeStatus(uint64_t now_ms, char *response,
                     size_t response_capacity);
    void queueAck(const char *op, StickOutputMode mode);
    void queueNack(const char *op, const char *reason);
    void checkControlBarrierTimeout(uint64_t now_ms);
    void finishPhysicalOverride(uint64_t now_ms);
    void finishAwaitingFresh(uint64_t now_ms);
    void clearSynthetic();
    static const char *modeName(StickOutputMode mode);
    static const char *sourceName(makcm::stick::StickSource source);
    static const char *stateName(makcm::stick::StickState state);

    makcm::stick::StickSession session_{};
    makcm::stick::ControllerReportCodec codec_{};
    makcm::stick::PhysicalPriorityOverride override_{};
    StickOutputMode mode_ = StickOutputMode::Mouse;
    PendingBarrier pending_barrier_ = PendingBarrier::None;
    uint64_t pending_barrier_started_ms_ = 0;
    uint8_t arming_completed_ = 0;
    uint32_t injection_counter_ = 0;
    uint64_t last_report_ms_ = 0;
    bool detach_requested_ = false;
    bool stream_fault_ = false;
    bool expiry_center_emitted_ = false;
    bool last_override_ = false;
    uint64_t override_started_ms_ = 0;
    bool last_awaiting_fresh_ = false;
    uint64_t awaiting_fresh_started_ms_ = 0;
    int32_t last_command_x_ = 0;
    int32_t last_command_y_ = 0;
    bool reversal_pending_x_ = false;
    bool reversal_pending_y_ = false;
    uint32_t reversal_epoch_x_ = 0;
    uint32_t reversal_epoch_y_ = 0;
    char format_id_[65]{};
    char cal_id_[65]{};
    char control_profile_[4]{'h', 'i', 'p', '\0'};
    char pending_response_[256]{};
    int16_t last_synthetic_x_ = 0;
    int16_t last_synthetic_y_ = 0;
    int16_t last_physical_x_ = 0;
    int16_t last_physical_y_ = 0;
    int16_t last_merged_x_ = 0;
    int16_t last_merged_y_ = 0;
    uint32_t last_emitted_sequence_ = 0;
    uint64_t last_emitted_accepted_ms_ = 0;
    uint64_t last_emitted_at_ms_ = 0;
    StickRuntimeCounters counters_{};
};

} // namespace makcm::native
