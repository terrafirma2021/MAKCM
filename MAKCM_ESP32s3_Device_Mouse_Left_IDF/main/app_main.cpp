#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tusb.h"

#include "bridge_transport.h"
#include "controller_profile_registry.h"
#include "endpoint_router.h"
#include "metrics.h"
#include "ota_update_manager.h"
#include "input_policy.h"
#include "profile_store.h"
#include "raw_proxy_driver.h"
#include "secondary.h"
#include "stick_runtime.h"
#include "tinyusb_adapter.h"
#include "OtaBoardConfig.h"
#include "OtaGeneration.h"
#include "PairOtaTransaction.h"
#include "OtaTrustAnchor.h"
#include "mbedtls/sha256.h"

namespace {
constexpr char kTag[] = "makcm-left-idf";

enum class ProxyState : uint8_t {
    WaitingProfile,
    ReceivingProfile,
    Enumerating,
    WaitingMount,
    Streaming,
    Disconnecting,
};

makcm::native::BridgeTransport bridge;
makcm::native::Secondary secondary;
makcm::native::ProfileStore profile;
makcm::native::Metrics metrics;
makcm::native::EndpointRouter router;
makcm::native::StickRuntime stick_runtime;
makcm::native::RawProxyDriver raw_driver;
makcm::native::InputPolicy input_policy;
makcm::native::OtaUpdateManager ota_manager;
SemaphoreHandle_t ota_mutex = nullptr;
std::atomic<bool> ota_quiesced{false};
std::atomic<bool> ota_ready{false};
std::atomic<bool> ota_boot_prerequisites_ready{false};
std::atomic<uint32_t> right_active_generation{0};
std::atomic<uint32_t> right_status_flags{0};
std::atomic<uint8_t> right_ota_state{
    static_cast<uint8_t>(makcm::ota::State::Fault)};
std::atomic<uint32_t> right_link_reset_at_ms{0};
// Set only after an exact terminal/cancellation response proves that no pair
// transaction exists.  It lets left-only/right-only maintenance sessions
// release without weakening the fail-closed pair-record gate.
std::atomic<bool> ota_unpaired_idle_release_authorized{false};
std::atomic<uint32_t> main_min_free_stack_bytes{UINT32_MAX};
makcm::ota::Frame pending_right_request{};
makcm::ota::Frame ota_control_response{};
// Control-plane scratch must not live on app_main's task stack.
char stick_status_response[makcm::native::kStickStatusCapacity]{};
bool pending_right_request_valid = false;
uint32_t pending_right_request_started_ms = 0;
uint32_t right_maintenance_transaction = 0;
bool right_response_expected = false;
uint32_t right_expected_transaction = 0;
uint32_t right_expected_sequence = 0;
// Last OTA_UNAVAILABLE the right board sent, and the hostState behind it.  A
// refused bootstrap reaches the host as a bare NotInMaintenance NACK, which
// says nothing about which of the right board's eight refusals fired.
uint8_t right_bootstrap_refusal_site = 0;
uint8_t right_bootstrap_refusal_state = 0;
uint8_t right_bootstrap_refusal_detail = 0;
// True once this maintenance session forwarded a right-targeted command that
// can leave an image in flight.  HELLO/ENTER/STATUS cannot, so a session that
// only ever forwarded those has nothing on the peer to wait for.
bool right_image_traffic_forwarded = false;
nvs_handle_t pair_nvs = 0;
makcm::ota::PairTransactionRecord pair_record{};
makcm::ota::PairTransactionRecord pair_candidate_a{};
makcm::ota::PairTransactionRecord pair_candidate_b{};
uint8_t pair_wire_a[makcm::ota::kPairRecordWireSize]{};
uint8_t pair_wire_b[makcm::ota::kPairRecordWireSize]{};
uint8_t pair_write_wire[makcm::ota::kPairRecordWireSize]{};
uint8_t pair_readback_wire[makcm::ota::kPairRecordWireSize]{};
uint8_t pair_legacy_probe[64]{};
int8_t pair_selected_copy = -1;
int32_t pair_last_nvs_error = 0;

// The pair record is a second durable store, entirely separate from the OTA
// snapshot, and it fails the host the same opaque way: every fault below
// becomes NackReason::FlashWriteFailed.  Worse, most of its faults never had an
// esp_err_t at all, so latchPairFault() invented ESP_ERR_INVALID_STATE /
// ESP_ERR_INVALID_CRC and collapsed a dozen distinct causes onto two values --
// a rejected record, a full partition, and an A/B conflict were indistinguish-
// able.  Name the site, exactly as SnapshotCommitFault does for the snapshot.
enum class PairFault : uint8_t {
    None = 0,
    StoreOpenFailed = 1,
    CopyReadFailed = 2,
    LegacyKeyPresent = 3,
    CopyConflict = 4,
    BothCopiesInvalid = 5,
    LoadedRecordInvalid = 6,
    NoNvsHandle = 7,
    SequenceSaturated = 8,
    RecordRejected = 9,
    EncodeFailed = 10,
    SetBlobFailed = 11,
    CommitFailed = 12,
    ReadbackMismatch = 13,
    ReadbackDecodeMismatch = 14,
    // 15+ are not storage faults at all: the record was never composed, or the
    // evidence it would be composed from did not survive validation.  Keeping
    // them in the same space as the write faults is the point -- the host sees
    // one FlashWriteFailed for both families and needs to tell them apart.
    TransactionSwitchBlocked = 15,
    SeedFactIncomplete = 16,
    BoardFactMismatch = 17,
    LocalStatusUnusable = 18,
    ResponseCorrelationMismatch = 19,
};
PairFault pair_fault_site = PairFault::None;

bool pair_record_valid = false;
bool pair_record_corrupt = false;
bool pair_mode_active = false;
makcm::ota::Frame pair_status_probe_request{};
makcm::ota::Frame pair_status_probe_response{};
uint32_t pair_status_probe_sequence = 1;
std::atomic<ProxyState> proxy_state{ProxyState::WaitingProfile};
std::atomic<uint32_t> active_session{0};
std::atomic<uint32_t> pending_session{0};
// ProfileBegin is retransmitted until the right host observes the ACK.  Keep
// the ACK edge-triggered per session; sending it again from the mounted path
// can make the right side reopen its streaming gate while EP0 is still
// processing the host's configuration requests.
std::atomic<uint32_t> accepted_session{0};
std::atomic<bool> usb_installed{false};
std::atomic<bool> usb_mounted{false};
std::atomic<bool> usb_suspended{false};
std::atomic<bool> remote_wakeup_enabled{false};
std::atomic<uint8_t> usb_install_error{0};
std::atomic<int32_t> usb_install_err_code{0};
std::atomic<uint32_t> usb_attach_events{0};
std::atomic<uint32_t> usb_detach_events{0};
std::atomic<uint32_t> usb_connect_calls{0};
std::atomic<uint8_t> dse_mask{0};
std::atomic<uint8_t> mouse_mask{0};
std::atomic<uint32_t> dse_last_report_ms{0};
makcm::hid::MouseReportLayout physical_mouse_layout{};
// DEBUG_STATUS includes topology, session and transfer counters and is longer
// than the normal ESPLOG line. Keep the complete line so CH343 diagnostics do
// not lose the connected/profile/ack fields at the front.
char bridge_text_line[768]{};
size_t bridge_text_length = 0;
bool bridge_text_valid = true;
std::atomic<uint32_t> bridge_text_reject{0};

void disconnectUsb();
bool handleOtaBridgeTextLine(const char *line);
void bridgeOtaFrame(void *, const makcm::ota::Frame &frame);
void secondaryOtaFrame(void *, const makcm::ota::Frame &frame);
void fillOtaControlResponse(const makcm::ota::Frame &request,
                            makcm::ota::Command command,
                            makcm::ota::State state,
                            makcm::ota::NackReason reason,
                            uint32_t next_offset,
                            uint32_t target_generation,
                            uint32_t active_generation,
                            uint32_t flags);

alignas(4) uint8_t fallback_langid[] = {4, 3, 0x09, 0x04};

void remoteWakeDeferred(void *) {
    raw_driver.remoteWakeup();
}

void requestRemoteWake() {
    if (usb_suspended.load(std::memory_order_acquire) &&
        remote_wakeup_enabled.load(std::memory_order_acquire)) {
        makcm::native::tinyusb::defer(&remoteWakeDeferred, nullptr);
    }
}

void secondaryMove(void *, int32_t x, int32_t y) {
    const uint64_t now_ms = static_cast<uint64_t>(esp_timer_get_time() / 1000ULL);
    if (router.stickSnapshot(now_ms).mode ==
        makcm::native::StickOutputMode::Stick) {
        // Phase 3 enables only explicit stick_state_q15. Legacy km.move stays
        // implemented and resumes unchanged in mouse mode; MOVE_COMPAT is a
        // later, separately leased source and must not mix with state_q15.
        return;
    }
    requestRemoteWake();
    router.mouseMove(x, y);
}

void secondaryWheel(void *, int32_t wheel) {
    requestRemoteWake();
    router.mouseMove(0, 0, wheel);
}

void secondaryButton(void *, uint8_t mask, bool pressed) {
    requestRemoteWake();
    router.mouseButton(mask, pressed);
}

void secondaryText(void *, const char *line) {
    if (line == nullptr) return;
    char stick_response[makcm::native::kStickStatusCapacity]{};
    if (router.handleStickLine(
            line, static_cast<uint64_t>(esp_timer_get_time() / 1000ULL),
            stick_response, sizeof(stick_response))) {
        if (stick_response[0] != '\0') secondary.sendText(stick_response);
        return;
    }
    // The host contract sends km.version() at the initial 115200 baud.  Keep
    // accepting the historical no-parentheses spelling as well because older
    // MAKCU clients used it during manual diagnostics.
    if (std::strcmp(line, "km.version()") == 0 ||
        std::strcmp(line, "km.version") == 0) {
        secondary.sendText("MAKCM native-left\r\n>>> ");
    } else if (std::strcmp(line, "km.echo(0)") == 0) {
        secondary.sendText("OK\r\n>>> ");
    } else if (std::strcmp(line, "LEFT_STATUS") == 0) {
        const auto validation = profile.validation();
        const auto snapshot = metrics.snapshot();
        uint16_t msos_length = 0;
        if (profile.stringDescriptor(0xEE, 0, &msos_length) == nullptr)
            msos_length = 0;
        char status[760]{};
        const int written = std::snprintf(
            status, sizeof(status),
            "LEFT_STATUS state=%u installed=%u mounted=%u profile=%u valid=%u "
            "vcode=%u vmsg=%s install=%u ierr=%ld pending=%08lx active=%08lx "
            "eps=%u rx=%lu rxerr=%lu txerr=%lu text=%lu "
            "seq=%lu/%lu/%lu/%lu/%lu/%lu reject=%lu "
            "ep=%lu/%lu/%lu/%lu/%lu "
             "reset=%u suspended=%u wake=%u cap=%u/%u cache=%u chit=%lu ctrl=%lu/%lu/%lu/%lu msos=%u "
             "sec=%lu/%lu/%lu/%lu/%u stack=%lu mstack=%lu bstack=%lu mouse=%u@%02x/%u inj=%lu/%lu btn=%lu/%lu\r\n",
            static_cast<unsigned>(proxy_state.load()),
            usb_installed.load() ? 1u : 0u, usb_mounted.load() ? 1u : 0u,
            profile.ready() ? 1u : 0u, validation.ok ? 1u : 0u,
            static_cast<unsigned>(validation.error_code), validation.message,
            static_cast<unsigned>(usb_install_error.load()),
            static_cast<long>(usb_install_err_code.load()),
            static_cast<unsigned long>(pending_session.load()),
            static_cast<unsigned long>(active_session.load()),
            static_cast<unsigned>(profile.endpointCount()),
            static_cast<unsigned long>(bridge.rxFrames()),
            static_cast<unsigned long>(bridge.rxErrors()),
             static_cast<unsigned long>(bridge.txErrors()),
             static_cast<unsigned long>(bridge.textBytes()),
             static_cast<unsigned long>(bridge.rxInputSequenceFrames()),
             static_cast<unsigned long>(bridge.rxInputSequenceGapFrames()),
             static_cast<unsigned long>(bridge.rxInputSequenceGapEvents()),
             static_cast<unsigned long>(bridge.rxInputSequenceDuplicates()),
             static_cast<unsigned long>(bridge.rxInputSequenceOutOfOrder()),
             static_cast<unsigned long>(bridge.rxInputSequenceSessionChanges()),
             static_cast<unsigned long>(snapshot.profile_reject),
            static_cast<unsigned long>(snapshot.endpoint_submit),
            static_cast<unsigned long>(snapshot.endpoint_complete),
             static_cast<unsigned long>(snapshot.endpoint_retry),
             static_cast<unsigned long>(snapshot.endpoint_drop),
             static_cast<unsigned long>(snapshot.endpoint_stale),
             static_cast<unsigned>(esp_reset_reason()),
             usb_suspended.load(std::memory_order_acquire) ? 1u : 0u,
             remote_wakeup_enabled.load(std::memory_order_acquire) ? 1u : 0u,
             static_cast<unsigned>(validation.capability),
            static_cast<unsigned>(validation.capability_reason),
            static_cast<unsigned>(profile.cachedControlCount()),
            static_cast<unsigned long>(raw_driver.controlCacheHits()),
            static_cast<unsigned long>(raw_driver.controlSetups()),
            static_cast<unsigned long>(raw_driver.controlForwards()),
            static_cast<unsigned long>(raw_driver.controlResponses()),
            static_cast<unsigned long>(raw_driver.controlStalls()),
            static_cast<unsigned>(msos_length),
            static_cast<unsigned long>(secondary.commands()),
            static_cast<unsigned long>(secondary.parseErrors()),
             static_cast<unsigned long>(secondary.txErrors()),
             static_cast<unsigned long>(secondary.rxOverflows()),
             secondary.fastBaud() ? 1u : 0u,
             static_cast<unsigned long>(secondary.minimumFreeTaskStackBytes()),
             static_cast<unsigned long>(
                 main_min_free_stack_bytes.load(std::memory_order_acquire)),
             static_cast<unsigned long>(bridge.minimumFreeRxTaskStackBytes()),
             router.mouseLayoutValid() ? 1u : 0u,
            static_cast<unsigned>(router.mouseLayoutReportId()),
            static_cast<unsigned>(router.mouseLayoutReportLength()),
            static_cast<unsigned long>(router.injectionMovesAccepted()),
            static_cast<unsigned long>(router.injectionMovesRejected()),
            static_cast<unsigned long>(router.injectionButtonsAccepted()),
            static_cast<unsigned long>(router.injectionButtonsRejected()));
        if (written > 0 && static_cast<size_t>(written) < sizeof(status))
            secondary.sendText(status);
        char usb_diag[420]{};
        const int diag_written = std::snprintf(
            usb_diag, sizeof(usb_diag),
            "LEFT_USB_DIAG attach=%lu detach=%lu connect=%lu "
            "open=%lu/%lu fail=%lu if=%u exp=%u ep=0x%02x reason=%u "
            "max=%u used=%u route=%lu/%lu rstage=%u rep=0x%02x cfg=%u\r\n",
            static_cast<unsigned long>(usb_attach_events.load()),
            static_cast<unsigned long>(usb_detach_events.load()),
            static_cast<unsigned long>(usb_connect_calls.load()),
            static_cast<unsigned long>(raw_driver.openSuccesses()),
            static_cast<unsigned long>(raw_driver.openCalls()),
            static_cast<unsigned long>(raw_driver.openFailures()),
            static_cast<unsigned>(raw_driver.lastOpenInterface()),
            static_cast<unsigned>(raw_driver.lastOpenExpectedEndpoints()),
            static_cast<unsigned>(raw_driver.lastOpenEndpoint()),
            static_cast<unsigned>(raw_driver.lastOpenReason()),
            static_cast<unsigned>(raw_driver.lastOpenMaxLen()),
            static_cast<unsigned>(raw_driver.lastOpenConsumed()),
            static_cast<unsigned long>(router.openAttempts()),
            static_cast<unsigned long>(router.openFailures()),
            static_cast<unsigned>(router.lastOpenFailureStage()),
            static_cast<unsigned>(router.lastOpenFailureEndpoint()),
            static_cast<unsigned>(profile.configurationDescriptorLength()));
        if (diag_written > 0 &&
            static_cast<size_t>(diag_written) < sizeof(usb_diag))
            secondary.sendText(usb_diag);
        // Every field the quiesce-release predicate actually reads.  A
        // fail-closed maintenance that refuses to release is a legitimate
        // outcome, but without this it has no observable cause: the board just
        // looks Idle and healthy while the data plane stays shut.
        char ota_diag[384]{};
        const int ota_written = std::snprintf(
            ota_diag, sizeof(ota_diag),
            "LEFT_OTA_DIAG quiesced=%u bridge_maint=%u sec_maint=%u "
            "state=%u excl=%u imgfwd=%u pending=%u reset_at=%lu "
            "authz=%u pair=%u/%u/%u rtx=%lu rexp=%u installed=%u "
            "nvs=%d ota=%d scf=%u pf=%u/%d prec=%lu/%u:%u/%u:%u "
            "rrefuse=%u/%u/%u\r\n",
            ota_quiesced.load(std::memory_order_acquire) ? 1u : 0u,
            bridge.otaMaintenance() ? 1u : 0u,
            secondary.otaMaintenance() ? 1u : 0u,
            static_cast<unsigned>(ota_manager.state()),
            ota_manager.maintenanceExclusive() ? 1u : 0u,
            right_image_traffic_forwarded ? 1u : 0u,
            pending_right_request_valid ? 1u : 0u,
            static_cast<unsigned long>(
                right_link_reset_at_ms.load(std::memory_order_acquire)),
            ota_unpaired_idle_release_authorized.load(
                std::memory_order_acquire) ? 1u : 0u,
            pair_record_valid ? 1u : 0u,
            static_cast<unsigned>(pair_record.stage),
            pair_record_corrupt ? 1u : 0u,
            static_cast<unsigned long>(right_maintenance_transaction),
            right_response_expected ? 1u : 0u,
            usb_installed.load(std::memory_order_acquire) ? 1u : 0u,
            // Every snapshot commit funnels through nvs_set_blob/commit/readback
            // and every one of them reports failure to the host as the single
            // opaque NackReason::FlashWriteFailed.  Without the underlying
            // esp_err_t a rejected MANIFEST is indistinguishable from a full
            // NVS partition, a failed readback, or a ledger-identity mismatch
            // that never touched flash at all.
            static_cast<int>(ota_manager.bootEvidence().last_nvs_error),
            static_cast<int>(ota_manager.bootEvidence().last_ota_error),
            // scf names which commit guard rejected the write; last_nvs_error
            // is only meaningful for scf=5/6/7, and is stale residue otherwise.
            static_cast<unsigned>(ota_manager.snapshotCommitFault()),
            // The pair record is the other durable store and reports the same
            // opaque FlashWriteFailed.  pf pairs the site with its esp_err_t
            // because most sites have no real one -- for those the code is a
            // stand-in and only the site is meaningful.
            static_cast<unsigned>(pair_fault_site),
            static_cast<int>(pair_last_nvs_error),
            // pair=/pf= say the record is unusable but not what it holds, and
            // the record's own transaction is what every mutation is judged
            // against -- rtx above is the right-board maintenance transaction,
            // a different number that reads deceptively like this one.  Which
            // participants are present decides whether a stalled stage is a
            // two-board transaction or a one-sided one, and those have
            // different exits.
            static_cast<unsigned long>(pair_record.transaction_id),
            pair_record.left.present ? 1u : 0u,
            static_cast<unsigned>(pair_record.left.local_phase),
            pair_record.right.present ? 1u : 0u,
            static_cast<unsigned>(pair_record.right.local_phase),
            // Which of the right board's eight bootstrap refusals fired, and
            // the hostState behind it.  The host only ever sees the resulting
            // NotInMaintenance NACK, which is the same for all eight.
            static_cast<unsigned>(right_bootstrap_refusal_site),
            static_cast<unsigned>(right_bootstrap_refusal_state),
            static_cast<unsigned>(right_bootstrap_refusal_detail));
        if (ota_written > 0 &&
            static_cast<size_t>(ota_written) < sizeof(ota_diag))
            secondary.sendText(ota_diag);
    } else if (std::strcmp(line, "LEFT_PAIR") == 0) {
        // Whether the record is displaceable decides whether any new
        // transaction can start, and it is decided by three predicates over
        // fields none of which are otherwise observable.  A blocked switch is
        // indistinguishable from a storage fault without them.
        auto dump_fact = [](const char *tag,
                            const makcm::ota::PairBoardFact &fact,
                            makcm::ota::Target role) {
            char buf[256]{};
            const int n = std::snprintf(
                buf, sizeof(buf),
                "LEFT_PAIR %s present=%u valid=%u idc=%u lv=%u pc=%u ph=%u "
                "tx=%lu gen=%lu seq=%lu/%lu cto=%u ctc=%u ctx=%lu cts=%lu "
                "slot=%u/%u/%u/%u imgeq=%u exact=%u\r\n",
                tag, fact.present ? 1u : 0u,
                makcm::ota::validPairBoardFact(fact, role) ? 1u : 0u,
                fact.identity_complete ? 1u : 0u,
                fact.local_validated ? 1u : 0u, fact.pair_committed ? 1u : 0u,
                static_cast<unsigned>(fact.local_phase),
                static_cast<unsigned long>(fact.transaction_id),
                static_cast<unsigned long>(fact.target_generation),
                static_cast<unsigned long>(fact.record_sequence),
                static_cast<unsigned long>(fact.last_request_sequence),
                static_cast<unsigned>(fact.completed_outcome),
                static_cast<unsigned>(fact.completed_terminal_command),
                static_cast<unsigned long>(fact.completed_transaction_id),
                static_cast<unsigned long>(fact.completed_terminal_sequence),
                static_cast<unsigned>(fact.target_slot),
                static_cast<unsigned>(fact.running_slot),
                static_cast<unsigned>(fact.boot_slot),
                static_cast<unsigned>(fact.previous_slot),
                std::memcmp(fact.running_image_sha256,
                            fact.previous_image_sha256, 32) == 0 ? 1u : 0u,
                makcm::ota::exactBoardRollbackFact(fact, role) ? 1u : 0u);
            if (n > 0 && static_cast<size_t>(n) < sizeof(buf))
                secondary.sendText(buf);
        };
        char head[192]{};
        const int head_n = std::snprintf(
            head, sizeof(head),
            "LEFT_PAIR record valid=%u corrupt=%u rec_valid=%u stage=%u "
            "tx=%lu gen=%lu seq=%lu commit=%u rollback=%u onesided=%u\r\n",
            pair_record_valid ? 1u : 0u, pair_record_corrupt ? 1u : 0u,
            makcm::ota::validPairRecord(pair_record) ? 1u : 0u,
            static_cast<unsigned>(pair_record.stage),
            static_cast<unsigned long>(pair_record.transaction_id),
            static_cast<unsigned long>(pair_record.target_generation),
            static_cast<unsigned long>(pair_record.record_sequence),
            makcm::ota::pairRecordHasExactPairCommit(pair_record) ? 1u : 0u,
            makcm::ota::pairRecordHasExactPairRollback(pair_record) ? 1u : 0u,
            makcm::ota::pairRecordHasExactOneSidedRollback(pair_record) ? 1u
                                                                       : 0u);
        if (head_n > 0 && static_cast<size_t>(head_n) < sizeof(head))
            secondary.sendText(head);
        dump_fact("left", pair_record.left, makcm::ota::Target::Left);
        dump_fact("right", pair_record.right, makcm::ota::Target::Right);
    } else if (std::strcmp(line, "LEFT_CFG") == 0) {
        const uint16_t length = profile.configurationDescriptorLength();
        char header[64]{};
        const int header_written = std::snprintf(
            header, sizeof(header), "LEFT_CFG length=%u\r\n",
            static_cast<unsigned>(length));
        if (header_written > 0 &&
            static_cast<size_t>(header_written) < sizeof(header))
            secondary.sendText(header);
        for (uint16_t offset = 0; offset < length; offset += 32) {
            char line_buf[120]{};
            int used = std::snprintf(line_buf, sizeof(line_buf),
                                     "LEFT_CFG off=%04x hex=", offset);
            const uint16_t count = std::min<uint16_t>(32, length - offset);
            for (uint16_t i = 0; i < count && used > 0 &&
                                static_cast<size_t>(used + 3) < sizeof(line_buf);
                 ++i) {
                used += std::snprintf(
                    line_buf + used, sizeof(line_buf) - used, "%02x",
                    profile.configurationDescriptor()[offset + i]);
            }
            if (used > 0 && static_cast<size_t>(used + 2) < sizeof(line_buf)) {
                line_buf[used++] = '\r';
                line_buf[used++] = '\n';
                line_buf[used] = '\0';
                secondary.sendText(line_buf);
            }
        }
        secondary.sendText("LEFT_CFG_END\r\n");
    } else if (std::strncmp(line, "DEBUG_", 6) == 0 ||
               std::strcmp(line, "READY") == 0) {
        // Preserve the legacy CH343 diagnostic contract: these commands are
        // handled by the right host and its ESPLOG response returns through
        // the mixed UART1 stream to bridgeText().
        char command[96]{};
        const int written = std::snprintf(command, sizeof(command), "%s\n",
                                           line);
        if (written > 0 && static_cast<size_t>(written) < sizeof(command))
            bridge.sendText(command);
    }
}

// The right host emits human-readable ESPLOG_* lines on the same UART as the
// binary protocol.  Keep those lines out of the binary decoder's hot path,
// but mirror them to the CH343 side so bring-up can distinguish "no device"
// from "profile rejected" without a second debug USB interface.
void bridgeText(void *, uint8_t byte) {
    if (byte == '\r') return;
    if (byte == '\n') {
        if (bridge_text_valid && bridge_text_length != 0) {
            bridge_text_line[bridge_text_length] = '\0';
            // Relay first, unconditionally.  OTA_* lines are the peer's half of
            // the maintenance handshake and used to be consumed in silence, so
            // a bootstrap that stalled on OTA_PENDING looked identical from the
            // host to a peer that never spoke at all.  The operator needs to
            // read what the right board actually said.
            secondary.sendText("RIGHT:");
            secondary.sendText(bridge_text_line);
            secondary.sendText("\r\n");
            handleOtaBridgeTextLine(bridge_text_line);
        } else if (!bridge_text_valid) {
            bridge_text_reject.fetch_add(1, std::memory_order_relaxed);
        }
        bridge_text_length = 0;
        bridge_text_valid = true;
        return;
    }
    // A real UART1 diagnostic line is printable ASCII.  If a lost COBS
    // delimiter makes binary report bytes appear as TextByte, discard that
    // entire pseudo-line instead of synchronously reflecting it to UART0 and
    // blocking the high-priority bridge RX task.
    if (byte < 0x20 || byte > 0x7e) {
        bridge_text_valid = false;
        return;
    }
    if (bridge_text_length + 1 >= sizeof(bridge_text_line)) {
        bridge_text_length = 0;
        bridge_text_valid = false;
        return;
    }
    if (bridge_text_valid)
        bridge_text_line[bridge_text_length++] = static_cast<char>(byte);
}

uint32_t nowMs() {
    return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
}

void publishPhysicalButtons() {
    secondary.setPhysicalMask(static_cast<uint8_t>(
        dse_mask.load(std::memory_order_acquire) |
        mouse_mask.load(std::memory_order_acquire)));
}

void observeControllerButtons(const uint8_t *report, uint16_t length) {
    const uint8_t old = dse_mask.load(std::memory_order_relaxed);
    uint8_t next = 0;
    if (!input_policy.decodeControllerButtons(report, length, old, next))
        return;
    dse_mask.store(next, std::memory_order_release);
    dse_last_report_ms.store(nowMs(), std::memory_order_release);
    publishPhysicalButtons();
}

void observeMouseButtons(const uint8_t *report, uint16_t length) {
    if (!physical_mouse_layout.valid || report == nullptr) return;
    const uint8_t buttons = makcm::hid::readMouseButtons(
        physical_mouse_layout, report, length);
    mouse_mask.store(buttons, std::memory_order_release);
    publishPhysicalButtons();
}

void inputMonitorTask(void *) {
    while (true) {
        const uint32_t last = dse_last_report_ms.load(std::memory_order_acquire);
        if (last != 0 && nowMs() - last > 100) {
            dse_last_report_ms.store(0, std::memory_order_release);
            if (dse_mask.exchange(0, std::memory_order_acq_rel) != 0)
                publishPhysicalButtons();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

bool sendProfileAccepted(uint32_t session) {
    if (session == 0 || !router.allEndpointsOpen()) {
        if (session != 0)
            ESP_LOGI(kTag, "PROFILE_WAIT_OPEN session=%08lx", static_cast<unsigned long>(session));
        return false;
    }
    uint32_t expected = 0;
    if (!accepted_session.compare_exchange_strong(
            expected, session, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        if (expected == session) return true;
        // A new profile session supersedes any stale acknowledgement left by
        // a previous disconnect.  Sessions are serialized by ProfileBegin.
        accepted_session.store(session, std::memory_order_release);
    }
    if (!bridge.sendEmpty(makcm::bridge::MessageType::ProfileAccepted,
                          session)) {
        accepted_session.store(0, std::memory_order_release);
        ESP_LOGW(kTag, "PROFILE_ACCEPTED queue full session=%08lx",
                 static_cast<unsigned long>(session));
        return false;
    }
    ESP_LOGI(kTag, "PROFILE_ACCEPTED session=%08lx endpoints=%u",
             static_cast<unsigned long>(session),
             static_cast<unsigned>(profile.endpointCount()));
    return true;
}

makcm::bridge::CapabilityReason profileRejectReason(
    const makcm::native::ProfileValidation &validation) {
    const uint8_t max_reason = static_cast<uint8_t>(
        makcm::bridge::CapabilityReason::BlobCrcMismatch);
    if (validation.capability_reason != 0 &&
        validation.capability_reason <= max_reason) {
        return static_cast<makcm::bridge::CapabilityReason>(
            validation.capability_reason);
    }
    switch (validation.error_code) {
    case 45:
    case 46:
    case 52:
        return makcm::bridge::CapabilityReason::BlobIncomplete;
    case 47:
    case 51:
        return makcm::bridge::CapabilityReason::BlobCrcMismatch;
    default:
        return makcm::bridge::CapabilityReason::DescriptorMalformed;
    }
}

void reportProfileReject(uint32_t session,
                         const makcm::native::ProfileValidation &validation) {
    if (session == 0) return;
    makcm::bridge::CapabilityResult result;
    result.capability = makcm::bridge::ProxyCapability::Unsupported;
    result.reason = profileRejectReason(validation);
    result.speed = profile.profile().usbSpeed;
    result.configurationCount = profile.profile().configurationCount;
    result.interfaceCount = validation.interface_count;
    result.endpointCount = validation.endpoint_count;
    result.inCount = validation.in_count;
    result.outCount = validation.out_count;
    result.maxEndpointNumber = static_cast<uint8_t>(std::min<uint16_t>(
        validation.max_endpoint_number, UINT8_MAX));
    result.maxPacketSize = validation.max_packet;
    result.estimatedBitsPerSecond = validation.estimated_bits_per_second;
    bridge.send(makcm::bridge::MessageType::CapabilityResult,
                reinterpret_cast<const uint8_t *>(&result), sizeof(result),
                session);
    char line[160]{};
    const int written = std::snprintf(
        line, sizeof(line),
        "UNSUPPORTED_PROFILE code=%u reason=%u text=%s\r\n",
        static_cast<unsigned>(validation.error_code),
        static_cast<unsigned>(result.reason), validation.message);
    if (written > 0 && static_cast<size_t>(written) < sizeof(line))
        secondary.sendText(line);
}

// esp_tinyusb owns tud_mount_cb()/tud_umount_cb() and dispatches these
// lifecycle events through tinyusb_config_t.event_cb.  Use that public event
// hook rather than defining the callbacks a second time (which would make the
// final link fail with duplicate symbols).
void usbEventCallback(tinyusb_event_t *event, void *) {
    if (event == nullptr) return;
    switch (event->id) {
    case TINYUSB_EVENT_ATTACHED: {
        if (!usb_installed.load(std::memory_order_acquire)) break;
        usb_attach_events.fetch_add(1, std::memory_order_relaxed);
        usb_mounted.store(true, std::memory_order_release);
        router.setUsbReady(!usb_suspended.load(std::memory_order_acquire));
        // A host bus reset can emit another ATTACHED/configured event without
        // a physical detach.  The existing session and endpoints are still
        // valid, so keep an active stream open instead of demoting it to
        // WaitingMount and waiting for an unnecessary second handshake.
        if (proxy_state.load(std::memory_order_acquire) ==
                ProxyState::Streaming) {
            break;
        }
        const uint32_t session = pending_session.load(std::memory_order_acquire);
        if (session != 0 && profile.configurationDescriptorLength() >= 6) {
            makcm::bridge::SetConfigurationHeader state;
            state.configuration = profile.configurationDescriptor()[5];
            bridge.send(makcm::bridge::MessageType::SetConfiguration,
                        reinterpret_cast<const uint8_t *>(&state),
                        sizeof(state), session);
        }
        const bool accepted = sendProfileAccepted(session);
        proxy_state.store(accepted ? ProxyState::WaitingMount
                                   : ProxyState::Enumerating,
                          std::memory_order_release);
        break;
    }
    case TINYUSB_EVENT_DETACHED:
        usb_detach_events.fetch_add(1, std::memory_order_relaxed);
        usb_mounted.store(false, std::memory_order_release);
        router.setUsbReady(false);
        if (pending_session.load(std::memory_order_acquire) != 0) {
            makcm::bridge::SetConfigurationHeader state;
            state.configuration = 0;
            bridge.send(makcm::bridge::MessageType::SetConfiguration,
                        reinterpret_cast<const uint8_t *>(&state),
                        sizeof(state), pending_session.load());
        }
        active_session.store(0, std::memory_order_release);
        accepted_session.store(0, std::memory_order_release);
        if (usb_installed.load(std::memory_order_acquire)) {
            proxy_state.store(ProxyState::WaitingMount,
                              std::memory_order_release);
        }
        break;
    case TINYUSB_EVENT_SUSPENDED:
        // Keep the event-hook view in sync with tud_suspend_cb().  The hook
        // is useful for diagnostics even when the USB stack delivers the
        // callback before the application task observes it.
        remote_wakeup_enabled.store(event->suspended.remote_wakeup,
                                    std::memory_order_release);
        usb_suspended.store(true, std::memory_order_release);
        // Do not submit new interrupt transfers while the host has suspended
        // the bus.  The bridge may continue receiving reports during this
        // interval; EndpointRouter keeps them in their per-endpoint queues,
        // and RESUMED re-enables the executor to drain them.  This avoids
        // turning a normal USB suspend into endpoint retries/drops and keeps
        // the pending relative-movement ledger intact for remote wakeup.
        router.setUsbReady(false);
        {
            makcm::bridge::SuspendResumeHeader state;
            state.suspended = 1;
            state.remoteWakeup = event->suspended.remote_wakeup ? 1 : 0;
            bridge.send(makcm::bridge::MessageType::SuspendResume,
                        reinterpret_cast<const uint8_t *>(&state),
                        sizeof(state), pending_session.load());
        }
        break;
    case TINYUSB_EVENT_RESUMED:
        usb_suspended.store(false, std::memory_order_release);
        router.setUsbReady(usb_mounted.load(std::memory_order_acquire));
        {
            makcm::bridge::SuspendResumeHeader state;
            state.suspended = 0;
            state.remoteWakeup = remote_wakeup_enabled.load() ? 1 : 0;
            bridge.send(makcm::bridge::MessageType::SuspendResume,
                        reinterpret_cast<const uint8_t *>(&state),
                        sizeof(state), pending_session.load());
        }
        break;
    default:
        break;
    }
}

bool installUsb() {
    if (usb_installed.load(std::memory_order_acquire)) return true;
    const auto validation = profile.validation();
    if (!validation.ok || validation.capability ==
            makcm::bridge::ProxyCapability::Unsupported) {
        ESP_LOGE(kTag, "profile capability rejected: code=%u reason=%u",
                 static_cast<unsigned>(validation.error_code),
                 static_cast<unsigned>(validation.capability_reason));
        usb_install_error.store(4, std::memory_order_release);
        return false;
    }
    usb_install_error.store(0, std::memory_order_release);
    usb_install_err_code.store(0, std::memory_order_release);
    input_policy.configure(&profile);
    if (!router.begin(&profile, &bridge, pending_session.load(), &metrics)) {
        usb_install_error.store(1, std::memory_order_release);
        metrics.add(makcm::native::MetricId::ProfileReject);
        return false;
    }
    const bool stick_profile_installed =
        makcm::native::installGeneratedControllerProfile(router, profile);
    ESP_LOGI(kTag, "direct-stick exact profiles=%u matched=%u",
             static_cast<unsigned>(makcm::native::generatedControllerProfileCount()),
             stick_profile_installed ? 1u : 0u);

    const auto *mouse_endpoint = profile.mouseEndpoint();
    if (mouse_endpoint != nullptr) {
        const auto *report = profile.hidReport(mouse_endpoint->interface_number);
        if (report != nullptr && report->data != nullptr) {
            makcm::hid::MouseReportLayout layout;
            if (makcm::hid::parseMouseReportDescriptor(
                    report->data, report->expected_length != 0
                        ? report->expected_length : report->received_length,
                    layout)) {
                physical_mouse_layout = layout;
                router.setMouseLayout(layout);
            } else {
                ESP_LOGW(kTag, "mouse HID report does not expose relative axes");
            }
        }
        if (!physical_mouse_layout.valid) {
            makcm::hid::MouseReportLayout fallback{};
            if (input_policy.fallbackMouseLayout(fallback)) {
                physical_mouse_layout = fallback;
                router.setMouseLayout(physical_mouse_layout);
                ESP_LOGW(kTag,
                         "using optional profile input policy mouse layout "
                         "report=0x%02x length=%u",
                         static_cast<unsigned>(fallback.reportId),
                         static_cast<unsigned>(fallback.reportLength));
            }
        }
    }
    if (!raw_driver.begin(&profile, &router, &bridge,
                          pending_session.load(), &metrics)) {
        usb_install_error.store(2, std::memory_order_release);
        return false;
    }

    tinyusb_config_t config = TINYUSB_DEFAULT_CONFIG();
    config.descriptor.device = reinterpret_cast<const tusb_desc_device_t *>(
        profile.deviceDescriptor());
    config.descriptor.full_speed_config = profile.configurationDescriptor();
#if defined(CONFIG_SOC_USB_OTG_SUPPORTED) && CONFIG_SOC_USB_OTG_SUPPORTED
    config.descriptor.high_speed_config = nullptr;
    config.descriptor.qualifier =
        profile.qualifierDescriptorLength() != 0
            ? reinterpret_cast<const tusb_desc_device_qualifier_t *>(
                  profile.qualifierDescriptor())
            : nullptr;
#endif
    // String descriptors are returned by tud_descriptor_string_cb so that the
    // proxy can preserve raw UTF-16 bytes and LANGID variants.  Leaving the
    // manager's string pointer null avoids a second, incompatible owner.
    config.descriptor.string = nullptr;
    config.descriptor.string_count = 0;
    config.task = TINYUSB_TASK_CUSTOM(6144, 20, 0);
    config.event_cb = &usbEventCallback;

    const esp_err_t err = tinyusb_driver_install(&config);
    if (err != ESP_OK) {
        usb_install_error.store(3, std::memory_order_release);
        usb_install_err_code.store(static_cast<int32_t>(err),
                                   std::memory_order_release);
        ESP_LOGE(kTag, "tinyusb_driver_install failed: %s", esp_err_to_name(err));
        raw_driver.reset();
        return false;
    }
    // The driver is installed before the host has selected the configuration.
    // Do not let the bridge open the input gate until TinyUSB has opened every
    // endpoint and invoked tud_mount_cb().
    router.setUsbReady(false);
    usb_installed.store(true, std::memory_order_release);
    usb_mounted.store(false, std::memory_order_release);
    usb_connect_calls.fetch_add(1, std::memory_order_relaxed);
    tud_connect();
    proxy_state.store(ProxyState::WaitingMount, std::memory_order_release);
    return true;
}

void disconnectUsb() {
    if (!usb_installed.exchange(false, std::memory_order_acq_rel)) return;
    router.setUsbReady(false);
    usb_suspended.store(false, std::memory_order_release);
    remote_wakeup_enabled.store(false, std::memory_order_release);
    accepted_session.store(0, std::memory_order_release);

    // `tud_disconnect()` drops D+ asynchronously.  Uninstalling the driver
    // in the same tick can make Windows see the next profile as a continuation
    // of the previous composite device, especially when a controller is
    // replaced by a keyboard or mouse.  Leave the pull-up detached for a
    // complete USB frame window before destroying the TinyUSB task/PHY.
    tud_disconnect();
    vTaskDelay(pdMS_TO_TICKS(50));
    usb_mounted.store(false, std::memory_order_release);
    // The esp_tinyusb public uninstall path stops the task and releases the
    // DWC2 instance.  A future hot-plug cycle can install a fresh descriptor
    // set without leaving old endpoint state behind.
    const esp_err_t uninstall_err = tinyusb_driver_uninstall();
    if (uninstall_err != ESP_OK && uninstall_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(kTag, "TinyUSB uninstall returned %s",
                 esp_err_to_name(uninstall_err));
    }
    raw_driver.reset();
    router.reset(0);
}

constexpr char kPairKeyA[] = "pair_a";
constexpr char kPairKeyB[] = "pair_b";

// Taking the site by parameter is deliberate: it makes every future fault path
// declare which one it is, instead of silently reusing a neighbour's code.
void latchPairFault(PairFault site, esp_err_t error) {
    pair_fault_site = site;
    pair_last_nvs_error = static_cast<int32_t>(error);
    pair_record_corrupt = true;
    ota_quiesced.store(true, std::memory_order_release);
    secondary.setOtaMaintenance(true);
}

bool readPairCopy(const char *key, uint8_t *wire,
                  makcm::ota::PairTransactionRecord &record, bool &present,
                  bool &valid) {
    present = false;
    valid = false;
    std::memset(wire, 0, makcm::ota::kPairRecordWireSize);
    size_t length = makcm::ota::kPairRecordWireSize;
    const esp_err_t result = nvs_get_blob(pair_nvs, key, wire, &length);
    pair_last_nvs_error = static_cast<int32_t>(result);
    if (result == ESP_ERR_NVS_NOT_FOUND) return true;
    present = true;
    if (result != ESP_OK || length != makcm::ota::kPairRecordWireSize)
        return result == ESP_ERR_NVS_INVALID_LENGTH;
    if (!makcm::ota::decodePairRecord(wire, length, record)) return true;
    valid = true;
    return true;
}

bool legacyPairKeyPresent() {
    size_t length = sizeof(pair_legacy_probe);
    std::memset(pair_legacy_probe, 0, sizeof(pair_legacy_probe));
    const esp_err_t result =
        nvs_get_blob(pair_nvs, "transaction", pair_legacy_probe, &length);
    pair_last_nvs_error = static_cast<int32_t>(result);
    if (result == ESP_ERR_NVS_NOT_FOUND) return false;
    // A V1 key is evidence, not permission to silently migrate.  Its schema
    // lacks image/slot identity, so leave the board fail-closed for host
    // reconciliation instead of erasing it.
    return true;
}

bool initPairStore() {
    const esp_err_t open_result = nvs_open("makcm_pair", NVS_READWRITE,
                                           &pair_nvs);
    if (open_result != ESP_OK) {
        latchPairFault(PairFault::StoreOpenFailed, open_result);
        return false;
    }

    bool present_a = false;
    bool present_b = false;
    bool valid_a = false;
    bool valid_b = false;
    if (!readPairCopy(kPairKeyA, pair_wire_a, pair_candidate_a, present_a,
                      valid_a) ||
        !readPairCopy(kPairKeyB, pair_wire_b, pair_candidate_b, present_b,
                      valid_b)) {
        latchPairFault(PairFault::CopyReadFailed,
                       static_cast<esp_err_t>(pair_last_nvs_error));
        return true;
    }
    if (legacyPairKeyPresent()) {
        latchPairFault(PairFault::LegacyKeyPresent, ESP_ERR_INVALID_VERSION);
        return true;
    }

    if (valid_a && valid_b &&
        pair_candidate_a.record_sequence == pair_candidate_b.record_sequence) {
        if (std::memcmp(pair_wire_a, pair_wire_b,
                        makcm::ota::kPairRecordWireSize) != 0) {
            latchPairFault(PairFault::CopyConflict, ESP_ERR_INVALID_CRC);
            return true;
        }
        pair_record = pair_candidate_a;
        pair_selected_copy = 0;
        pair_record_valid = true;
    } else if (valid_a &&
               (!valid_b || pair_candidate_a.record_sequence >
                                pair_candidate_b.record_sequence)) {
        pair_record = pair_candidate_a;
        pair_selected_copy = 0;
        pair_record_valid = true;
    } else if (valid_b) {
        pair_record = pair_candidate_b;
        pair_selected_copy = 1;
        pair_record_valid = true;
    } else if (present_a || present_b) {
        latchPairFault(PairFault::BothCopiesInvalid, ESP_ERR_INVALID_CRC);
        return true;
    } else {
        pair_record = {};
        pair_selected_copy = -1;
        pair_record_valid = false;
        pair_record_corrupt = false;
        pair_fault_site = PairFault::None;
        pair_last_nvs_error = ESP_ERR_NVS_NOT_FOUND;
        return true;
    }

    if (!makcm::ota::validPairRecord(pair_record)) {
        latchPairFault(PairFault::LoadedRecordInvalid, ESP_ERR_INVALID_CRC);
        return true;
    }
    pair_mode_active = pair_record.stage != makcm::ota::PairStage::None;
    pair_record_corrupt = false;
    pair_fault_site = PairFault::None;
    // Only unfinished records are maintenance hand-offs.  Exact terminal
    // commit/rollback tombstones survive reboot for reconciliation but are
    // positive evidence that the normal data plane may start immediately.
    ota_quiesced.store(
        makcm::ota::pairRecordRequiresHostReconcile(pair_record),
        std::memory_order_release);
    return true;
}

bool persistPairRecord(makcm::ota::PairTransactionRecord next) {
    if (pair_nvs == 0) {
        latchPairFault(PairFault::NoNvsHandle, ESP_ERR_INVALID_STATE);
        return false;
    }
    // A mutation normally starts from a copy of pair_record, so its incoming
    // sequence is the *old* sequence rather than zero.  Always allocate the
    // next sequence here; otherwise alternating A/B writes contain different
    // bytes with the same sequence and boot recovery correctly flags them as
    // a conflict.
    if (pair_record_valid && pair_record.record_sequence == UINT32_MAX) {
        latchPairFault(PairFault::SequenceSaturated, ESP_ERR_INVALID_STATE);
        return false;
    }
    next.record_sequence = pair_record_valid
                               ? pair_record.record_sequence + 1
                               : 1;
    if (next.completed.present && next.completed.record_sequence == 0)
        next.completed.record_sequence = next.record_sequence;
    makcm::ota::sealPairRecord(next);
    // Split from the encode below on purpose.  This is the pair-record twin of
    // SnapshotCommitFault::EncodeRejected: the record the board built fails its
    // own validator before any flash is touched, which is a logic fault in
    // whoever composed the mutation -- not a storage problem.  Sharing one code
    // with a genuine encode failure is what made that class of bug read as
    // flash trouble in the OTA snapshot path for three sessions.
    if (!makcm::ota::validPairRecord(next)) {
        latchPairFault(PairFault::RecordRejected, ESP_ERR_INVALID_STATE);
        return false;
    }
    if (!makcm::ota::encodePairRecord(next, pair_write_wire,
                                      sizeof(pair_write_wire))) {
        latchPairFault(PairFault::EncodeFailed, ESP_ERR_INVALID_SIZE);
        return false;
    }

    const int8_t write_copy = pair_selected_copy == 0 ? 1 : 0;
    const char *key = write_copy == 0 ? kPairKeyA : kPairKeyB;
    esp_err_t result = nvs_set_blob(pair_nvs, key, pair_write_wire,
                                    sizeof(pair_write_wire));
    if (result != ESP_OK) {
        latchPairFault(PairFault::SetBlobFailed, result);
        return false;
    }
    result = nvs_commit(pair_nvs);
    if (result != ESP_OK) {
        latchPairFault(PairFault::CommitFailed, result);
        return false;
    }
    size_t read_length = sizeof(pair_readback_wire);
    std::memset(pair_readback_wire, 0, sizeof(pair_readback_wire));
    result = nvs_get_blob(pair_nvs, key, pair_readback_wire, &read_length);
    if (result != ESP_OK || read_length != sizeof(pair_readback_wire) ||
        std::memcmp(pair_readback_wire, pair_write_wire,
                    sizeof(pair_write_wire)) != 0) {
        latchPairFault(PairFault::ReadbackMismatch,
                       result == ESP_OK ? ESP_ERR_INVALID_CRC : result);
        return false;
    }
    makcm::ota::PairTransactionRecord verified{};
    if (!makcm::ota::decodePairRecord(pair_readback_wire, read_length,
                                      verified) ||
        verified.record_sequence != next.record_sequence) {
        latchPairFault(PairFault::ReadbackDecodeMismatch, ESP_ERR_INVALID_CRC);
        return false;
    }
    pair_record = verified;
    pair_selected_copy = write_copy;
    pair_record_valid = true;
    pair_record_corrupt = false;
    pair_fault_site = PairFault::None;
    pair_mode_active = verified.stage != makcm::ota::PairStage::None;
    ota_quiesced.store(true, std::memory_order_release);
    return true;
}

makcm::ota::PairCompletedTombstone makePairTombstone(
    const makcm::ota::PairBoardFact &left,
    const makcm::ota::PairBoardFact &right, makcm::ota::snapshot::Outcome outcome,
    makcm::ota::Command command, uint32_t sequence) {
    makcm::ota::PairCompletedTombstone completed{};
    completed.present = true;
    completed.outcome = outcome;
    completed.terminal_command = command;
    completed.transaction_id = left.transaction_id;
    completed.target_generation = left.target_generation;
    completed.terminal_sequence = sequence;
    completed.left_target_slot = left.target_slot;
    completed.right_target_slot = right.target_slot;
    std::memcpy(completed.left_image_sha256, left.image_sha256, 32);
    std::memcpy(completed.right_image_sha256, right.image_sha256, 32);
    std::memcpy(completed.left_manifest_sha256, left.manifest_sha256, 32);
    std::memcpy(completed.right_manifest_sha256, right.manifest_sha256, 32);
    std::memcpy(completed.left_identity_digest, left.identity_digest, 32);
    std::memcpy(completed.right_identity_digest, right.identity_digest, 32);
    return completed;
}

// `transaction_id` is the transaction this mutation is about, and the caller
// always knows it -- from the request frame, or from the freshly-read status
// that produced the fact.  It used to be inferred instead, from whichever of
// the two facts was present with right preferred, which is wrong whenever a
// caller re-supplies a carried-over peer fact from the record: the stale peer
// always won the preference, captured the record's identity, and then the
// genuinely fresh fact was rejected against it as BoardFactMismatch.  A new
// transaction could therefore never be recognised as a switch, so the switch
// guard below never even ran.
bool mutatePairRecord(uint32_t transaction_id, makcm::ota::PairStage stage,
                      const makcm::ota::PairBoardFact *left,
                      const makcm::ota::PairBoardFact *right,
                      const makcm::ota::PairCompletedTombstone *completed) {
    if (transaction_id == 0) {
        latchPairFault(PairFault::SeedFactIncomplete, ESP_ERR_INVALID_ARG);
        return false;
    }
    // A fact from another transaction is ordinary staleness, not an error:
    // callers pass the record's own peer fact forward without re-checking it.
    // Drop it here so BoardFactMismatch keeps its narrow meaning -- a fact that
    // claims this transaction but disagrees about its generation.
    if (left != nullptr &&
        (!left->present || left->transaction_id != transaction_id))
        left = nullptr;
    if (right != nullptr &&
        (!right->present || right->transaction_id != transaction_id))
        right = nullptr;

    makcm::ota::PairTransactionRecord next{};
    if (pair_record_valid) {
        next = pair_record;
    }
    if (pair_record_valid && next.transaction_id != 0 &&
        next.transaction_id != transaction_id) {
        // The record holds a different transaction.  It may only be displaced
        // once it owes nothing: an exact pair commit, an exact pair rollback,
        // or a rollback the peer never joined.  That last shape has no other
        // exit -- RolledBack requires both facts -- so evaluating retirability
        // here means every new transaction is an opportunity to clear it,
        // rather than leaving the record wedged with no way back.
        if (!makcm::ota::pairRecordHasExactPairCommit(pair_record) &&
            !makcm::ota::pairRecordHasExactPairRollback(pair_record) &&
            !makcm::ota::pairRecordHasExactOneSidedRollback(pair_record)) {
            latchPairFault(PairFault::TransactionSwitchBlocked, ESP_ERR_INVALID_STATE);
            return false;
        }
        const auto completed_history = pair_record.completed;
        next = {};
        next.completed = completed_history;
    }
    if (next.transaction_id == 0) {
        const makcm::ota::PairBoardFact *seed = left != nullptr ? left : right;
        if (seed == nullptr || seed->target_generation == 0) {
            latchPairFault(PairFault::SeedFactIncomplete, ESP_ERR_INVALID_ARG);
            return false;
        }
        next.transaction_id = transaction_id;
        next.target_generation = seed->target_generation;
    }
    if (left != nullptr) {
        if (left->target_generation != next.target_generation) {
            latchPairFault(PairFault::BoardFactMismatch, ESP_ERR_INVALID_STATE);
            return false;
        }
        next.left = *left;
    }
    if (right != nullptr) {
        if (right->target_generation != next.target_generation) {
            latchPairFault(PairFault::BoardFactMismatch, ESP_ERR_INVALID_STATE);
            return false;
        }
        next.right = *right;
    }
    if (completed != nullptr && completed->present) next.completed = *completed;
    next.stage = stage;
    if (!persistPairRecord(next)) return false;
    return true;
}

bool pairFactFromStatus(const makcm::ota::StatusPayloadV2 &status,
                        makcm::ota::Target expected_role,
                        makcm::ota::PairBoardFact &fact) {
    using namespace makcm::ota;
    const uint16_t evidence = status.evidence_flags;
    const bool active = (evidence & kEvidenceActivePresent) != 0;
    const bool completed = (evidence & kEvidenceCompletedPresent) != 0;
    if (status.role != expected_role || (!active && !completed) ||
        status.record_sequence == 0)
        return false;
    std::memset(&fact, 0, sizeof(fact));
    fact.present = true;
    fact.role = status.role;
    fact.identity_complete =
        (evidence & kEvidenceIdentityDigestValid) != 0 &&
        (status.target_slot != 0xFF) && (status.running_slot != 0xFF) &&
        (status.boot_slot != 0xFF) &&
        !snapshot::allZero(status.image_sha256, 32) &&
        !snapshot::allZero(status.manifest_sha256, 32) &&
        !snapshot::allZero(status.identity_digest, 32);
    fact.local_validated = (evidence & kEvidenceLocalValidated) != 0;
    fact.pair_committed = (evidence & kEvidencePairCommitted) != 0;
    fact.local_phase = active
                           ? status.local_phase
                           : (status.completed_outcome ==
                                      snapshot::Outcome::Confirmed
                                  ? snapshot::Phase::PairCommitted
                                  : snapshot::Phase::RolledBack);
    fact.image_state = status.image_state;
    fact.running_slot = status.running_slot;
    fact.boot_slot = status.boot_slot;
    fact.target_slot = status.target_slot;
    fact.previous_slot = status.previous_slot;
    fact.last_command = status.last_command;
    fact.last_response_command = status.last_response_command;
    fact.last_error = status.reason;
    fact.completed_outcome = status.completed_outcome;
    fact.completed_terminal_command = status.completed_terminal_command;
    fact.protocol_epoch = status.protocol_epoch;
    fact.transaction_id = active ? status.record_transaction_id
                                 : status.completed_transaction_id;
    fact.target_generation = status.target_generation;
    fact.previous_generation = status.previous_generation;
    fact.partition_schema = status.partition_schema;
    fact.image_size = status.image_size;
    fact.record_sequence = status.record_sequence;
    fact.last_request_sequence = status.last_request_sequence;
    fact.completed_transaction_id = status.completed_transaction_id;
    fact.completed_terminal_sequence = status.completed_terminal_sequence;
    if (fact.last_request_sequence == 0)
        fact.last_request_sequence = fact.completed_terminal_sequence;
    if (active && fact.completed_transaction_id != fact.transaction_id) {
        fact.completed_outcome = makcm::ota::snapshot::Outcome::None;
        fact.completed_terminal_command = static_cast<makcm::ota::Command>(0);
        fact.completed_transaction_id = 0;
        fact.completed_terminal_sequence = 0;
    }
    fact.reset_reason = status.reset_reason;
    fact.last_ota_error = status.last_ota_error;
    fact.last_state_query_error = status.last_state_query_error;
    fact.boot_attempt = status.boot_attempt;
    std::memcpy(fact.image_sha256, status.image_sha256, 32);
    std::memcpy(fact.manifest_sha256, status.manifest_sha256, 32);
    std::memcpy(fact.running_image_sha256, status.running_image_sha256, 32);
    std::memcpy(fact.identity_digest, status.identity_digest, 32);
    std::memcpy(fact.previous_image_sha256, status.previous_image_sha256, 32);
    std::memcpy(fact.hardware_revision, status.hardware_revision,
                sizeof(fact.hardware_revision));
    std::memcpy(fact.project_name, status.project_name,
                sizeof(fact.project_name));
    std::memcpy(fact.app_version, status.app_version,
                sizeof(fact.app_version));
    return validPairBoardFact(fact, expected_role);
}

bool requestLocalStatusV2(const makcm::ota::Frame &request,
                          makcm::ota::StatusPayloadV2 &status) {
    std::memset(&pair_status_probe_request, 0,
                sizeof(pair_status_probe_request));
    std::memset(&pair_status_probe_response, 0,
                sizeof(pair_status_probe_response));
    // This is an in-process request to the left-board OTA participant.  Host
    // is a valid response-routing target, not a valid request target for the
    // left manager; using Host here makes every local Status V2 refresh fail
    // with WrongTarget and strands the pair coordinator after PREPARE/ACTIVATE.
    pair_status_probe_request.target = makcm::ota::Target::Left;
    pair_status_probe_request.command = makcm::ota::Command::Status;
    pair_status_probe_request.flags = makcm::ota::kFlagStatusV2;
    pair_status_probe_request.transaction_id = request.transaction_id;
    pair_status_probe_request.sequence = pair_status_probe_sequence++;
    if (pair_status_probe_sequence == 0 || pair_status_probe_sequence == UINT32_MAX)
        pair_status_probe_sequence = 1;
    if (!ota_manager.handle(pair_status_probe_request, pair_status_probe_response,
                            nowMs()) ||
        (pair_status_probe_response.flags & makcm::ota::kFlagStatusV2) == 0)
        return false;
    return makcm::ota::decodeStatusV2(pair_status_probe_response.payload,
                                      pair_status_probe_response.length,
                                      status);
}

bool statusHasNoTransactionEvidence(
    const makcm::ota::StatusPayloadV2 &status) {
    return (status.evidence_flags &
            (makcm::ota::kEvidenceActivePresent |
             makcm::ota::kEvidenceCompletedPresent)) == 0 &&
           status.target_generation == 0 &&
           (status.state == makcm::ota::State::Idle ||
            status.state == makcm::ota::State::Maintenance);
}

bool pairReadyForLeftConfirm() {
    if (!pair_record_valid || pair_record_corrupt ||
        !pair_record.left.present || !pair_record.right.present)
        return false;
    const auto &left = pair_record.left;
    const auto &right = pair_record.right;
    return left.identity_complete && right.identity_complete &&
           left.local_validated && right.local_validated &&
           right.pair_committed &&
           left.transaction_id == pair_record.transaction_id &&
           right.transaction_id == pair_record.transaction_id &&
           left.target_generation == pair_record.target_generation &&
           right.target_generation == pair_record.target_generation &&
           right.running_slot == right.target_slot &&
           right.boot_slot == right.target_slot && right.image_state == 2;
}

bool preparePairIntentForRequest(const makcm::ota::Frame &request) {
    using namespace makcm::ota;
    if (request.target != Target::Left && request.target != Target::Right)
        return true;
    const Target role = request.target;
    if (request.command == Command::Manifest) {
        // Each board's local A/B snapshot is the intent that must precede
        // esp_ota_begin.  A pair intent here would be written before the board
        // validates signature/hardware/project and lets a rejected manifest
        // poison pair_a/pair_b indefinitely.  Record the board in the pair
        // coordinator only after an exact Status V2 fact proves acceptance.
        return true;
    }
    if (!pair_mode_active || !pair_record_valid) return true;

    if (request.command == Command::Activate) {
        PairBoardFact fact{};
        if (role == Target::Left) {
            // pair-prepare's final Status V2 can be lost together with the
            // host process.  Refresh the left participant directly before
            // persisting ACTIVATE intent instead of assuming the coordinator
            // already cached that post-PREPARE fact.
            StatusPayloadV2 local_status{};
            if (!requestLocalStatusV2(request, local_status) ||
                !pairFactFromStatus(local_status, Target::Left, fact))
                return false;
        } else {
            fact = pair_record.right;
        }
        if (!fact.present || !fact.identity_complete ||
            fact.transaction_id != request.transaction_id ||
            fact.target_generation != pair_record.target_generation)
            return false;
        if (role == Target::Left &&
            (!pair_record.right.present ||
             !pair_record.right.identity_complete ||
             !pair_record.right.local_validated ||
             pair_record.right.transaction_id != request.transaction_id ||
             pair_record.right.target_generation !=
                 pair_record.target_generation))
            return false;
        fact.local_phase = snapshot::Phase::ActivateIntent;
        fact.last_command = Command::Activate;
        fact.last_request_sequence = request.sequence;
        return mutatePairRecord(request.transaction_id,
                                role == Target::Right
                                    ? PairStage::RightActivateIntent
                                    : PairStage::LeftActivateIntent,
                                role == Target::Left ? &fact :
                                                        (pair_record.left.present
                                                             ? &pair_record.left
                                                             : nullptr),
                                role == Target::Right ? &fact :
                                                         (pair_record.right.present
                                                              ? &pair_record.right
                                                              : nullptr),
                                nullptr);
    }
    if (request.command == Command::Confirm) {
        if (role == Target::Right) {
            // Right confirms first.  At this point both images must already
            // be locally validated, but right.pair_committed is expected to
            // still be false: that fact is produced by the response to this
            // very command, which the host observes directly now that the
            // bridge relays right-board responses verbatim.
            if (!pair_record.left.present || !pair_record.right.present ||
                !pair_record.left.identity_complete ||
                !pair_record.right.identity_complete ||
                !pair_record.left.local_validated ||
                !pair_record.right.local_validated ||
                pair_record.left.transaction_id != request.transaction_id ||
                pair_record.right.transaction_id != request.transaction_id ||
                pair_record.left.target_generation !=
                    pair_record.target_generation ||
                pair_record.right.target_generation !=
                    pair_record.target_generation)
                return false;
            return mutatePairRecord(request.transaction_id,
                                    PairStage::PairCommitIntent,
                                    &pair_record.left, &pair_record.right,
                                    nullptr);
        }
        // The left ACTIVATE ACK is emitted before restart, so the coordinator
        // copy of the left fact is necessarily pre-validation at that point.
        // Refresh it from the local Status V2 authority before committing the
        // pair intent; otherwise Confirm can never pass after a normal reboot.
        makcm::ota::StatusPayloadV2 local_status{};
        makcm::ota::PairBoardFact local_fact{};
        if (!requestLocalStatusV2(request, local_status) ||
            !pairFactFromStatus(local_status, Target::Left, local_fact) ||
            !pair_record_valid || !pair_record.right.present ||
            !local_fact.local_validated ||
            !pair_record.right.local_validated ||
            !pair_record.right.pair_committed ||
            local_fact.transaction_id != request.transaction_id ||
            pair_record.right.transaction_id != request.transaction_id ||
            local_fact.target_generation != pair_record.target_generation ||
            pair_record.right.target_generation !=
                pair_record.target_generation)
            return false;
        return mutatePairRecord(request.transaction_id,
                                PairStage::PairCommitIntent, &local_fact,
                                &pair_record.right, nullptr);
    }
    if (request.command == Command::Abort) {
        // An ABORT naming a transaction no board ever joined has no pair intent
        // to record -- most often the recovery ABORT, which invents a fresh id
        // precisely because the board holds no usable one.  Writing rollback
        // intent for it would mean inventing a participant; refusing it would
        // reject the one command that exists to clear a stuck board.  Neither:
        // leave the record untouched and let the abort proceed.  Any record it
        // leaves standing is still displaceable by the next transaction that
        // does produce a fact.
        const bool joined =
            pair_record_valid &&
            ((pair_record.left.present &&
              pair_record.left.transaction_id == request.transaction_id) ||
             (pair_record.right.present &&
              pair_record.right.transaction_id == request.transaction_id));
        if (!joined) return true;
        return mutatePairRecord(request.transaction_id,
                                PairStage::RollbackIntent,
                                pair_record.left.present ? &pair_record.left
                                                         : nullptr,
                                pair_record.right.present ? &pair_record.right
                                                          : nullptr,
                                nullptr);
    }
    return true;
}

bool updatePairFromLocalStatus(const makcm::ota::Frame &request,
                               makcm::ota::PairStage fallback_stage) {
    makcm::ota::StatusPayloadV2 status{};
    makcm::ota::PairBoardFact left{};
    if (!requestLocalStatusV2(request, status) ||
        !pairFactFromStatus(status, makcm::ota::Target::Left, left)) {
        latchPairFault(PairFault::LocalStatusUnusable, ESP_ERR_INVALID_RESPONSE);
        return false;
    }
    // Only a peer fact from *this* transaction may inform the stage.  A
    // carried-over one answers questions like both_rolled_back and
    // right->local_validated about a transaction that is already over.
    const makcm::ota::PairBoardFact *right =
        pair_record_valid && pair_record.right.present &&
                pair_record.right.transaction_id == left.transaction_id
            ? &pair_record.right
            : nullptr;
    makcm::ota::PairStage stage = fallback_stage;
    makcm::ota::PairCompletedTombstone completed{};
    const makcm::ota::PairCompletedTombstone *completed_ptr = nullptr;
    const bool both_committed = right != nullptr && right->pair_committed &&
                                left.pair_committed;
    const bool both_rolled_back =
        right != nullptr &&
        right->local_phase == makcm::ota::snapshot::Phase::RolledBack &&
        left.local_phase == makcm::ota::snapshot::Phase::RolledBack;
    if (both_rolled_back) {
        stage = makcm::ota::PairStage::RolledBack;
        completed = makePairTombstone(
            left, *right, makcm::ota::snapshot::Outcome::RolledBack,
            makcm::ota::Command::Abort,
            std::max(left.completed_terminal_sequence != 0
                         ? left.completed_terminal_sequence
                         : left.last_request_sequence,
                     right->completed_terminal_sequence != 0
                         ? right->completed_terminal_sequence
                         : right->last_request_sequence));
        completed_ptr = &completed;
    } else if (left.local_phase ==
                   makcm::ota::snapshot::Phase::RolledBack ||
               left.local_phase ==
                   makcm::ota::snapshot::Phase::RollbackIntent) {
        stage = makcm::ota::PairStage::RollbackIntent;
    } else if (left.local_phase == makcm::ota::snapshot::Phase::Failed) {
        stage = makcm::ota::PairStage::MaintenanceRequired;
    } else if (both_committed) {
        stage = makcm::ota::PairStage::BothPairCommitted;
    } else if (right != nullptr && right->local_validated &&
               left.local_validated) {
        stage = both_committed ? makcm::ota::PairStage::BothPairCommitted
                               : makcm::ota::PairStage::BothLocalValidated;
    } else if (left.local_phase ==
                   makcm::ota::snapshot::Phase::ActivateIntent ||
               left.local_phase ==
                   makcm::ota::snapshot::Phase::BootSelected ||
               left.local_phase ==
                   makcm::ota::snapshot::Phase::PendingVerify) {
        stage = right != nullptr && right->local_validated
                    ? makcm::ota::PairStage::LeftActivateIntent
                    : makcm::ota::PairStage::MaintenanceRequired;
    } else if (left.local_phase == makcm::ota::snapshot::Phase::Receiving) {
        stage = makcm::ota::PairStage::LeftPrepareIntent;
    } else if (left.local_phase == makcm::ota::snapshot::Phase::Prepared) {
        if (right != nullptr && right->local_validated)
            stage = makcm::ota::PairStage::RightLocalValidated;
        else if (right != nullptr &&
                 right->local_phase ==
                     makcm::ota::snapshot::Phase::Prepared)
            stage = makcm::ota::PairStage::BothPrepared;
        else
            stage = makcm::ota::PairStage::MaintenanceRequired;
    } else {
        stage = makcm::ota::PairStage::MaintenanceRequired;
    }
    if (both_committed && completed_ptr == nullptr) {
        completed = makePairTombstone(
            left, *right, makcm::ota::snapshot::Outcome::Confirmed,
            makcm::ota::Command::Confirm, request.sequence);
        completed_ptr = &completed;
    }
    return mutatePairRecord(left.transaction_id, stage, &left, right,
                            completed_ptr);
}

bool observeLocalOtaResponse(const makcm::ota::Frame &request,
                             makcm::ota::Frame &response) {
    if (response.transaction_id != request.transaction_id ||
        response.sequence != request.sequence) {
        latchPairFault(PairFault::ResponseCorrelationMismatch, ESP_ERR_INVALID_RESPONSE);
        fillOtaControlResponse(request, makcm::ota::Command::Nack,
                               makcm::ota::State::Fault,
                               makcm::ota::NackReason::WrongTransaction, 0, 0,
                               makcm::ota::build::kPairGeneration,
                               makcm::ota::kStatusMaintenanceExclusive);
        return false;
    }
    if (response.command == makcm::ota::Command::StatusResponse &&
        request.command == makcm::ota::Command::Status &&
        (response.flags & makcm::ota::kFlagStatusV2) != 0) {
        makcm::ota::StatusPayloadV2 status{};
        if (!makcm::ota::decodeStatusV2(response.payload, response.length,
                                        status) ||
            status.last_command != request.command ||
            status.last_response_command != response.command) {
            latchPairFault(PairFault::ResponseCorrelationMismatch, ESP_ERR_INVALID_RESPONSE);
            return false;
        }
        // An idle board with no local evidence is a normal discovery or a
        // rejected-MANIFEST recovery fact.  If only the right participant has
        // joined, preserve that right fact instead of manufacturing a left
        // participant or poisoning the coordinator.
        if (statusHasNoTransactionEvidence(status)) {
            const bool no_active_pair =
                !pair_record_valid ||
                pair_record.stage == makcm::ota::PairStage::None;
            if ((!pair_mode_active && no_active_pair) ||
                (pair_record_valid && !pair_record.left.present))
                return true;
        }
        // A lone left transaction must never START pair mode.  The prepare
        // flow's verification STATUS carries Prepared evidence; recording it
        // as BothPrepared asserts a right participant that never joined, and
        // the ACTIVATE pair gate then demands an impossible right fact
        // (single-board left OTA NACKs CompatibilityRejected forever).  Only
        // refresh facts once pair mode is already active.
        if (!pair_mode_active) return true;
        makcm::ota::PairBoardFact left{};
        if (!pairFactFromStatus(status, makcm::ota::Target::Left, left)) {
            latchPairFault(PairFault::LocalStatusUnusable, ESP_ERR_INVALID_RESPONSE);
            return false;
        }
        // Reuse the same phase-aware transition mapping used after lifecycle
        // ACKs.  A Receiving/Failed/Rollback fact must never be mislabeled as
        // BothPrepared merely because the STATUS request succeeded.
        return updatePairFromLocalStatus(request,
                                         makcm::ota::PairStage::BothPrepared);
    }
    if (response.command != makcm::ota::Command::Ack) return true;
    if (!pair_mode_active ||
        (request.command != makcm::ota::Command::Prepare &&
         request.command != makcm::ota::Command::Activate &&
         request.command != makcm::ota::Command::Confirm &&
         request.command != makcm::ota::Command::Abort))
        return true;

    makcm::ota::PairStage fallback = makcm::ota::PairStage::BothPrepared;
    if (request.command == makcm::ota::Command::Activate)
        fallback = makcm::ota::PairStage::LeftActivateIntent;
    else if (request.command == makcm::ota::Command::Confirm)
        fallback = makcm::ota::PairStage::BothPairCommitted;
    else if (request.command == makcm::ota::Command::Abort)
        fallback = makcm::ota::PairStage::RollbackIntent;
    if (!updatePairFromLocalStatus(request, fallback)) {
        fillOtaControlResponse(request, makcm::ota::Command::Nack,
                               makcm::ota::State::Fault,
                               makcm::ota::NackReason::FlashWriteFailed, 0, 0,
                               makcm::ota::build::kPairGeneration,
                               makcm::ota::kStatusMaintenanceExclusive);
        return false;
    }
    // Preserve rollback evidence; never erase pair_a/pair_b.
    return true;
}

void fillOtaControlResponse(const makcm::ota::Frame &request,
                            makcm::ota::Command command,
                            makcm::ota::State state,
                            makcm::ota::NackReason reason,
                            uint32_t next_offset,
                            uint32_t target_generation,
                            uint32_t active_generation,
                            uint32_t flags) {
    // Aggregate assignment materializes a full 4 KiB Frame temporary on the
    // caller stack. Zero the static control buffer in place instead.
    std::memset(&ota_control_response, 0, sizeof(ota_control_response));
    ota_control_response.target = request.target;
    ota_control_response.command = command;
    ota_control_response.flags = makcm::ota::kFlagResponse;
    ota_control_response.transaction_id = request.transaction_id;
    ota_control_response.offset = next_offset;
    ota_control_response.sequence = request.sequence;
    ota_control_response.length = makcm::ota::kStatusPayloadSize;
    makcm::ota::StatusPayload payload{};
    payload.state = state;
    payload.last_command = request.command;
    payload.reason = reason;
    payload.next_offset = next_offset;
    payload.target_generation = target_generation;
    payload.active_generation = active_generation;
    payload.status_flags = flags;
    makcm::ota::encodeStatusPayload(payload, ota_control_response.payload);
}

void sendOtaNack(const makcm::ota::Frame &request,
                 makcm::ota::NackReason reason) {
    fillOtaControlResponse(
        request, makcm::ota::Command::Nack, makcm::ota::State::Fault,
        reason, 0, 0, makcm::ota::build::kPairGeneration,
        ota_quiesced.load(std::memory_order_acquire)
            ? static_cast<uint32_t>(
                  makcm::ota::kStatusMaintenanceExclusive)
            : 0u);
    secondary.sendOtaFrame(ota_control_response);
}

bool otaQuiesce(void *) {
    ota_unpaired_idle_release_authorized.store(false,
                                                std::memory_order_release);
    ota_quiesced.store(true, std::memory_order_release);
    // Arm the release cadence from the moment the data plane closes.  The
    // retry loop only runs while right_link_reset_at_ms is non-zero, so a
    // quiesce whose right board never answers would otherwise never even
    // evaluate tryReleaseOtaQuiesce().  The predicate there does all the
    // gating; arming early only decides when it is asked.
    right_link_reset_at_ms.store(nowMs() + 250, std::memory_order_release);
    secondary.setOtaMaintenance(true);
    proxy_state.store(ProxyState::Disconnecting, std::memory_order_release);
    if (usb_installed.load(std::memory_order_acquire)) disconnectUsb();
    active_session.store(0, std::memory_order_release);
    pending_session.store(0, std::memory_order_release);
    accepted_session.store(0, std::memory_order_release);
    return !usb_installed.load(std::memory_order_acquire);
}

// Release the two UART maintenance gates only after the local updater and
// pair transaction have both reached a completed state.  This is deliberately
// separate from otaQuiesce(): a right-board STATUS/CONFIRM response may arm
// right_link_reset_at_ms, but a failed bootstrap that never quiesced the
// whole proxy must not accidentally reopen the data plane.
bool tryReleaseOtaQuiesce() {
    if (!ota_quiesced.load(std::memory_order_acquire) ||
        usb_installed.load(std::memory_order_acquire) ||
        ota_manager.maintenanceExclusive() ||
        pair_record_corrupt || pending_right_request_valid) {
        return false;
    }

    // Retain the exact completed tombstone for lost-ACK and reboot
    // reconciliation.  A pair update releases only from its exact tombstone.
    // A board-only/empty session may release only after the exact response
    // path below explicitly authorizes an Idle/no-pair state.
    const bool exact_pair_terminal =
        pair_record_valid &&
        (makcm::ota::pairRecordHasExactPairCommit(pair_record) ||
         makcm::ota::pairRecordHasExactPairRollback(pair_record));
    const bool exact_unpaired_idle =
        (!pair_record_valid ||
         pair_record.stage == makcm::ota::PairStage::None) &&
        !pair_record_corrupt &&
        ota_unpaired_idle_release_authorized.load(std::memory_order_acquire) &&
        ota_manager.state() == makcm::ota::State::Idle;
    // A quiesce armed purely to bootstrap the right board must not be able to
    // outlive that bootstrap.  otaQuiesce() closes the CH343 text plane
    // (Secondary drops every text line while its maintenance bit is set), and
    // an Idle left board holds no transaction, so the manager NACKs ABORT as
    // WrongTransaction.  Once the right side stops answering there is then
    // nothing the host can send that reopens the data plane, and only a
    // physical power cycle recovers it -- with Windows passthrough down the
    // whole time.
    //
    // Waiting for a peer fact is only meaningful once something was forwarded
    // that the peer could still be executing.  HELLO/ENTER/STATUS cannot leave
    // an image in flight, so a session that forwarded nothing else has no peer
    // work to wait for and the local Idle facts are the complete truth.
    const bool bootstrap_only_session =
        !right_image_traffic_forwarded &&
        ota_manager.state() == makcm::ota::State::Idle &&
        (!pair_record_valid ||
         pair_record.stage == makcm::ota::PairStage::None);
    if (!exact_pair_terminal && !exact_unpaired_idle && !bootstrap_only_session)
        return false;

    if (!bridge.setOtaMaintenance(false)) return false;
    secondary.setOtaMaintenance(false);
    std::memset(&pending_right_request, 0, sizeof(pending_right_request));
    pending_right_request_valid = false;
    pending_right_request_started_ms = 0;
    right_maintenance_transaction = 0;
    right_response_expected = false;
    right_expected_transaction = 0;
    right_expected_sequence = 0;
    right_image_traffic_forwarded = false;
    active_session.store(0, std::memory_order_release);
    pending_session.store(0, std::memory_order_release);
    accepted_session.store(0, std::memory_order_release);
    right_link_reset_at_ms.store(0, std::memory_order_release);
    ota_unpaired_idle_release_authorized.store(false,
                                                std::memory_order_release);
    ota_quiesced.store(false, std::memory_order_release);
    proxy_state.store(ProxyState::WaitingProfile, std::memory_order_release);
    return true;
}

bool otaPairHealth(void *) {
    // Pair health is a fact gate, not a generation/flag hint.  Before left
    // Confirm, right must already have its exact committed tombstone and both
    // local-valid facts; release uses the stricter both-committed predicate.
    // A lone left transaction never joined the pair coordinator (see the
    // observeLocalOtaResponse guard), so it has no pair fact to satisfy --
    // demanding one made single-board left Confirm structurally impossible.
    if (!pair_mode_active) return bridge.running();
    return pairReadyForLeftConfirm() && bridge.running();
}

bool otaLocalHealth(void *) {
    // This gate is set only after NVS, pair transaction recovery, trust anchor
    // and the OTA local record have all initialized. Controller, UART1 and
    // peer state remain pair/data-plane concerns and cannot kill this slot.
    return ota_boot_prerequisites_ready.load(std::memory_order_acquire) &&
           ota_manager.state() != makcm::ota::State::Fault;
}

bool forwardRightOtaRequest(const makcm::ota::Frame &request) {
    switch (request.command) {
    case makcm::ota::Command::Manifest:
    case makcm::ota::Command::Data:
    case makcm::ota::Command::Prepare:
    case makcm::ota::Command::Activate:
    case makcm::ota::Command::Confirm:
    case makcm::ota::Command::Abort:
        right_image_traffic_forwarded = true;
        break;
    default:
        break;
    }
    right_response_expected = true;
    right_expected_transaction = request.transaction_id;
    right_expected_sequence = request.sequence;
    if (bridge.sendOta(request)) return true;
    right_response_expected = false;
    return false;
}

bool startRightMaintenanceBootstrap(const makcm::ota::Frame &request) {
    if (request.transaction_id == 0) return false;
    ota_unpaired_idle_release_authorized.store(false,
                                                std::memory_order_release);
    if (pending_right_request_valid) {
        // OtaClient retransmits the same sequence after a timeout.  The right
        // USB Host may still be completing its bounded teardown, so an exact
        // duplicate must remain pending instead of being converted to BUSY.
        return request.transaction_id == pending_right_request.transaction_id &&
               request.command == pending_right_request.command &&
               request.sequence == pending_right_request.sequence;
    }
    char command[96]{};
    const int length = std::snprintf(
        command, sizeof(command), "OTA_MAINT_BEGIN %lu %u\n",
        static_cast<unsigned long>(request.transaction_id),
        static_cast<unsigned>(makcm::ota::build::kPairGeneration));
    // sendOtaText(), not sendText(): this runs precisely when maintenance is
    // held for a different transaction, and sendText() refuses to transmit in
    // that state -- which made a stale maintenance latch permanently
    // unrecoverable from the host.
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(command) ||
        !bridge.sendOtaText(command)) return false;
    pending_right_request = request;
    pending_right_request_valid = true;
    pending_right_request_started_ms = nowMs();
    // right_maintenance_transaction is latched only on OTA_READY: a refused
    // bootstrap must not displace the transaction the right board is actually
    // bound to, or the forward gate and the peer diverge with no exit.
    return true;
}

bool handleOtaBridgeTextLine(const char *line) {
    if (line == nullptr || std::strncmp(line, "OTA_", 4) != 0) return false;
    if (ota_mutex == nullptr ||
        xSemaphoreTake(ota_mutex, pdMS_TO_TICKS(250)) != pdTRUE) return true;
    // Right-board USB teardown is asynchronous.  Keep the original pending
    // binary request alive while the right RX task drains DeviceGoneAccepted;
    // only OTA_READY or OTA_UNAVAILABLE completes the bootstrap contract.
    if (std::strcmp(line, "OTA_PENDING") == 0) {
        xSemaphoreGive(ota_mutex);
        return true;
    }
    if (std::strncmp(line, "OTA_UNAVAILABLE", 15) == 0) {
        // The right board now names which of its eight refusals fired and the
        // hostState behind it.  Latch both: this line is the only evidence of
        // why a bootstrap died, and the NACK the host receives cannot carry it.
        right_bootstrap_refusal_site = 0;
        right_bootstrap_refusal_state = 0;
        right_bootstrap_refusal_detail = 0;
        {
            unsigned site = 0;
            unsigned state = 0;
            unsigned detail = 0;
            if (std::sscanf(line, "OTA_UNAVAILABLE %u %u %u", &site, &state,
                            &detail) == 3) {
                right_bootstrap_refusal_site = static_cast<uint8_t>(site);
                right_bootstrap_refusal_state = static_cast<uint8_t>(state);
                right_bootstrap_refusal_detail = static_cast<uint8_t>(detail);
            }
        }
        // Explicit refuse must never fall into the READY path's otaQuiesce
        // attempt — that mutates local exclusive state on a negative reply.
        if (pending_right_request_valid) {
            sendOtaNack(pending_right_request,
                        makcm::ota::NackReason::NotInMaintenance);
            pending_right_request_valid = false;
            pending_right_request_started_ms = 0;
        }
        xSemaphoreGive(ota_mutex);
        return true;
    }
    unsigned long transaction = 0;
    char tail = '\0';
    const bool ready = std::sscanf(line, "OTA_READY %lu%c", &transaction,
                                   &tail) == 1 &&
                       transaction != 0 && transaction <= UINT32_MAX &&
                       pending_right_request_valid &&
                       transaction == pending_right_request.transaction_id;
    // Keep the mixed bridge alive until the right board has really completed
    // USB Host teardown.  Quiescing earlier hides DeviceGone/diagnostic
    // traffic and can strand the bootstrap with no recovery response.
    if (!ready || !otaQuiesce(nullptr) || !bridge.setOtaMaintenance(true)) {
        if (pending_right_request_valid) {
            sendOtaNack(pending_right_request,
                        makcm::ota::NackReason::NotInMaintenance);
            pending_right_request_valid = false;
            pending_right_request_started_ms = 0;
        }
        xSemaphoreGive(ota_mutex);
        return true;
    }
    right_maintenance_transaction = pending_right_request.transaction_id;

    if (pending_right_request.command ==
        makcm::ota::Command::EnterMaintenance) {
        fillOtaControlResponse(
            pending_right_request, makcm::ota::Command::Ack,
            makcm::ota::State::Maintenance, makcm::ota::NackReason::None,
            0, 0, 0, makcm::ota::kStatusMaintenanceExclusive);
        secondary.sendOtaFrame(ota_control_response);
    } else if (!preparePairIntentForRequest(pending_right_request)) {
        sendOtaNack(pending_right_request,
                    makcm::ota::NackReason::CompatibilityRejected);
    } else if (!forwardRightOtaRequest(pending_right_request)) {
        sendOtaNack(pending_right_request, makcm::ota::NackReason::Busy);
    }
    pending_right_request_valid = false;
    pending_right_request_started_ms = 0;
    xSemaphoreGive(ota_mutex);
    return true;
}

void bridgeOtaFrame(void *, const makcm::ota::Frame &frame) {
    if (ota_mutex == nullptr ||
        xSemaphoreTake(ota_mutex, pdMS_TO_TICKS(250)) != pdTRUE) return;
    const bool exact_response = right_response_expected &&
        frame.target == makcm::ota::Target::Right &&
        (frame.flags & makcm::ota::kFlagResponse) != 0 &&
        frame.transaction_id == right_expected_transaction &&
        frame.sequence == right_expected_sequence;
    if (!exact_response) {
        xSemaphoreGive(ota_mutex);
        return;
    }
    right_response_expected = false;
    // Host-driven relay: the host owns response interpretation. Decode Status
    // V2 only to refresh cached right-board diagnostics and the release
    // predicates below; never veto, persist, or replace the peer's answer.
    // Vetoing here manufactured a V1 NACK the Status-V2 host filter drops
    // (host sees only a timeout) and latched a quiesce with no exit.
    makcm::ota::StatusPayloadV2 status_v2{};
    const bool valid_status_v2 =
        frame.command == makcm::ota::Command::StatusResponse &&
        (frame.flags & makcm::ota::kFlagStatusV2) != 0 &&
        makcm::ota::decodeStatusV2(frame.payload, frame.length, status_v2);
    if (valid_status_v2) {
        right_ota_state.store(static_cast<uint8_t>(status_v2.state),
                              std::memory_order_release);
        right_active_generation.store(status_v2.active_generation,
                                      std::memory_order_release);
        right_status_flags.store(status_v2.status_flags,
                                 std::memory_order_release);
    }
    const bool idle_maintenance_status = valid_status_v2 &&
        status_v2.state == makcm::ota::State::Maintenance &&
        status_v2.target_generation == 0 &&
         (status_v2.status_flags &
          (makcm::ota::kStatusPrepared |
           makcm::ota::kStatusPendingVerify)) == 0;
    const bool exact_pair_completed = valid_status_v2 &&
        makcm::ota::pairRecordHasExactPairCommit(pair_record) &&
        status_v2.completed_transaction_id == pair_record.transaction_id &&
        status_v2.completed_outcome ==
            makcm::ota::snapshot::Outcome::Confirmed;
    const bool exact_unpaired_terminal =
        (!pair_record_valid ||
         pair_record.stage == makcm::ota::PairStage::None) &&
        !pair_record_corrupt &&
        idle_maintenance_status;
    if (exact_unpaired_terminal) {
        ota_unpaired_idle_release_authorized.store(
            true, std::memory_order_release);
    }
    // A V1 ACK carries no transaction/image identity, so it can never prove a
    // terminal outcome or authorize a release.  V1 remains readable purely for
    // discovery of an old peer; only exact Status V2 facts release the bridge.
    if (idle_maintenance_status || exact_pair_completed) {
            // ACTIVATE is deliberately excluded.  The right board reboots
            // into AwaitConfirm while retaining exclusive binary UART1
            // ownership; releasing the bridge after 250 ms would make the
            // host's post-reboot STATUS fall back to a text bootstrap that
            // the right receiver cannot consume.  Keep the binary channel
            // until left activation/reboot restores it from the pair record.
            right_link_reset_at_ms.store(nowMs() + 250,
                                         std::memory_order_release);
    }
    secondary.sendOtaFrame(frame);
    xSemaphoreGive(ota_mutex);
}

void secondaryOtaFrame(void *, const makcm::ota::Frame &frame) {
    if (ota_mutex == nullptr ||
        xSemaphoreTake(ota_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return;
    if (frame.target == makcm::ota::Target::Left) {
        if (frame.command != makcm::ota::Command::Hello &&
            frame.command != makcm::ota::Command::Status &&
            frame.command != makcm::ota::Command::Abort &&
            frame.command != makcm::ota::Command::Confirm) {
            ota_unpaired_idle_release_authorized.store(
                false, std::memory_order_release);
        }
        if (!ota_ready.load(std::memory_order_acquire)) {
            sendOtaNack(frame, makcm::ota::NackReason::RollbackUnavailable);
        } else if (!preparePairIntentForRequest(frame)) {
            sendOtaNack(frame, makcm::ota::NackReason::CompatibilityRejected);
        } else {
            // Preserve pair evidence until rollback is durably observable.
            // Clearing it before the rollback call makes ACK loss and a reset
            // indistinguishable from a completed rollback.
            ota_manager.handle(frame, ota_control_response, nowMs());
            (void)observeLocalOtaResponse(frame, ota_control_response);
            const bool exact_pair_terminal =
                pair_record_valid &&
                (makcm::ota::pairRecordHasExactPairCommit(pair_record) ||
                 makcm::ota::pairRecordHasExactPairRollback(pair_record));
            if (exact_pair_terminal) {
                // The right CONFIRM ACK may have been lost.  In that replay
                // path the final left fact is what completes the pair record,
                // so arm release here instead of relying on another right
                // response that may never arrive.
                right_link_reset_at_ms.store(nowMs() + 250,
                                             std::memory_order_release);
            }
            const bool exact_unpaired_terminal =
                (!pair_record_valid ||
                 pair_record.stage == makcm::ota::PairStage::None) &&
                !pair_record_corrupt &&
                ota_control_response.command == makcm::ota::Command::Ack &&
                ota_control_response.transaction_id == frame.transaction_id &&
                ota_control_response.sequence == frame.sequence &&
                (frame.command == makcm::ota::Command::Abort ||
                 frame.command == makcm::ota::Command::Confirm) &&
                ota_manager.state() == makcm::ota::State::Idle;
            if (exact_unpaired_terminal) {
                ota_unpaired_idle_release_authorized.store(
                    true, std::memory_order_release);
                right_link_reset_at_ms.store(nowMs() + 250,
                                             std::memory_order_release);
            }
            secondary.sendOtaFrame(ota_control_response);
        }
    } else if (frame.target == makcm::ota::Target::Right) {
        if (bridge.otaMaintenance() &&
            frame.transaction_id == right_maintenance_transaction) {
            if (!preparePairIntentForRequest(frame))
                sendOtaNack(frame, makcm::ota::NackReason::CompatibilityRejected);
            else if (!forwardRightOtaRequest(frame))
                sendOtaNack(frame, makcm::ota::NackReason::Busy);
        } else if (frame.command == makcm::ota::Command::EnterMaintenance ||
                   frame.command == makcm::ota::Command::Hello ||
                   frame.command == makcm::ota::Command::Status) {
            if (!startRightMaintenanceBootstrap(frame))
                sendOtaNack(frame, makcm::ota::NackReason::Busy);
        } else {
            sendOtaNack(frame, makcm::ota::NackReason::NotInMaintenance);
        }
    } else {
        sendOtaNack(frame, makcm::ota::NackReason::WrongTarget);
    }
    xSemaphoreGive(ota_mutex);
}

void handleBridgeFrame(void *, const makcm::bridge::Frame &frame) {
    if (ota_quiesced.load(std::memory_order_acquire)) return;
    const ProxyState state = proxy_state.load(std::memory_order_acquire);
    switch (frame.type) {
    case makcm::bridge::MessageType::ProfileBegin: {
        if (frame.length != sizeof(makcm::bridge::DeviceProfile) ||
            frame.session == 0) break;
        if (state != ProxyState::WaitingProfile) {
            const uint32_t current_session = pending_session.load();
            if (frame.session != current_session) break;
            // A retransmitted profile is expected while the right side waits
            // for the acknowledgement.  Never tear down a device that is
            // already enumerating; just repeat the ACK when it is mounted.
            if (usb_installed.load(std::memory_order_acquire)) {
                if (usb_mounted.load(std::memory_order_acquire)) {
                    sendProfileAccepted(current_session);
                }
                break;
            }
            // A ProfileReady/CapabilityResult frame can be lost after one
            // or more blob chunks. The right side then retransmits the
            // complete profile with the same session. The old code ignored
            // that ProfileBegin while ReceivingProfile, leaving the left
            // side with an incomplete bitmap forever because blob chunks are
            // append-only. Restart this incomplete receive transaction so
            // retransmitted chunks can be consumed from offset zero. This
            // does not affect an installed/mounted session (handled above).
            if (state == ProxyState::ReceivingProfile) {
                ESP_LOGW(kTag,
                         "restarting incomplete profile session=%08lx",
                         static_cast<unsigned long>(frame.session));
            }
        }
        if (usb_installed.load()) disconnectUsb();
        makcm::bridge::DeviceProfile incoming;
        std::memcpy(&incoming, frame.payload, sizeof(incoming));
        if (!profile.begin(incoming)) {
            metrics.add(makcm::native::MetricId::ProfileReject);
            proxy_state.store(ProxyState::WaitingProfile);
            break;
        }
        pending_session.store(frame.session, std::memory_order_release);
        active_session.store(0, std::memory_order_release);
        router.bindStickPhysical(
            frame.session,
            static_cast<uint64_t>(esp_timer_get_time() / 1000ULL));
        accepted_session.store(0, std::memory_order_release);
        dse_mask.store(0, std::memory_order_release);
        mouse_mask.store(0, std::memory_order_release);
        dse_last_report_ms.store(0, std::memory_order_release);
        physical_mouse_layout = {};
        publishPhysicalButtons();
        proxy_state.store(ProxyState::ReceivingProfile, std::memory_order_release);
        break;
    }
    case makcm::bridge::MessageType::ProfileDeviceDescriptorChunk:
        if (state == ProxyState::ReceivingProfile &&
            frame.session == pending_session.load() && frame.length >= 2) {
            const uint16_t offset = static_cast<uint16_t>(frame.payload[0]) |
                                    (static_cast<uint16_t>(frame.payload[1]) << 8);
            profile.appendDeviceDescriptor(offset, frame.payload + 2,
                                           frame.length - 2);
        }
        break;
    case makcm::bridge::MessageType::ProfileConfigurationChunk:
        if (state == ProxyState::ReceivingProfile &&
            frame.session == pending_session.load() && frame.length >= 2) {
            const uint16_t offset = static_cast<uint16_t>(frame.payload[0]) |
                                    (static_cast<uint16_t>(frame.payload[1]) << 8);
            profile.appendConfigurationDescriptor(offset, frame.payload + 2,
                                                  frame.length - 2);
        }
        break;
    case makcm::bridge::MessageType::ProfileDescriptorChunk:
        if (state == ProxyState::ReceivingProfile &&
            frame.session == pending_session.load() && frame.length >= 3) {
            const uint16_t offset = static_cast<uint16_t>(frame.payload[1]) |
                                    (static_cast<uint16_t>(frame.payload[2]) << 8);
            profile.appendHidReportDescriptor(frame.payload[0], offset,
                                              frame.payload + 3,
                                              frame.length - 3);
        }
        break;
    case makcm::bridge::MessageType::ProfileStringDescriptorChunk:
        if (state == ProxyState::ReceivingProfile &&
            frame.session == pending_session.load() && frame.length >= 5) {
            const uint16_t langid = static_cast<uint16_t>(frame.payload[1]) |
                                    (static_cast<uint16_t>(frame.payload[2]) << 8);
            const uint16_t offset = static_cast<uint16_t>(frame.payload[3]) |
                                    (static_cast<uint16_t>(frame.payload[4]) << 8);
            profile.appendStringDescriptor(frame.payload[0], langid, offset,
                                           frame.payload + 5, frame.length - 5);
        }
        break;
    case makcm::bridge::MessageType::ProfileControlCacheChunk:
        if (state == ProxyState::ReceivingProfile &&
            frame.session == pending_session.load() &&
            frame.length >= sizeof(makcm::bridge::ProfileControlCacheHeader)) {
            makcm::bridge::ProfileControlCacheHeader header{};
            std::memcpy(&header, frame.payload, sizeof(header));
            profile.appendControlCache(
                header, frame.payload + sizeof(header),
                static_cast<uint16_t>(frame.length - sizeof(header)));
        }
        break;
    case makcm::bridge::MessageType::ProfileBlobChunk:
        if (state == ProxyState::ReceivingProfile &&
            frame.session == pending_session.load() &&
            frame.length >= sizeof(makcm::bridge::ProfileBlobChunkHeader)) {
            makcm::bridge::ProfileBlobChunkHeader header{};
            std::memcpy(&header, frame.payload, sizeof(header));
            profile.appendProfileBlob(
                header, frame.payload + sizeof(header),
                static_cast<uint16_t>(frame.length - sizeof(header)));
        }
        break;
    case makcm::bridge::MessageType::CapabilityResult:
        if (state == ProxyState::ReceivingProfile &&
            frame.session == pending_session.load() &&
            frame.length == sizeof(makcm::bridge::CapabilityResult)) {
            makcm::bridge::CapabilityResult result{};
            std::memcpy(&result, frame.payload, sizeof(result));
            profile.setCapabilityResult(result);
        }
        break;
    case makcm::bridge::MessageType::ProfileReady: {
        if (state != ProxyState::ReceivingProfile ||
            frame.session != pending_session.load()) break;
        const auto validation = profile.validate();
        if (!validation.ok || !installUsb()) {
            metrics.add(makcm::native::MetricId::ProfileReject);
            if (!validation.ok) {
                reportProfileReject(frame.session, validation);
            } else {
                secondary.sendText("USB_INSTALL_REJECT\r\n");
            }
            proxy_state.store(ProxyState::WaitingProfile, std::memory_order_release);
        }
        break;
    }
    case makcm::bridge::MessageType::EndpointInData:
    case makcm::bridge::MessageType::EndpointIn:
        if (state == ProxyState::Streaming &&
            frame.session == active_session.load()) {
            const bool generic = frame.type ==
                makcm::bridge::MessageType::EndpointInData;
            const uint16_t metadata = generic ? 2 : 1;
            if (frame.length <= metadata) break;
            const uint8_t address = frame.payload[0];
            const uint8_t *report = frame.payload + metadata;
            const uint16_t report_length =
                static_cast<uint16_t>(frame.length - metadata);
            if ((address & 0x80) == 0) break;
            router.queueEndpointInput(address, report, report_length);
            const auto *mouse_endpoint = profile.mouseEndpoint();
            if (input_policy.isControllerEndpoint(address)) {
                observeControllerButtons(report, report_length);
            }
            if (mouse_endpoint != nullptr &&
                address == mouse_endpoint->address)
                observeMouseButtons(report, report_length);
        }
        break;
    case makcm::bridge::MessageType::ControlInResult:
    case makcm::bridge::MessageType::ControlResponse:
        if (frame.session == active_session.load() ||
            frame.session == pending_session.load()) {
            raw_driver.onControlResponse(frame.payload, frame.length);
        }
        break;
    case makcm::bridge::MessageType::StreamStarted:
        if (usb_mounted.load(std::memory_order_acquire) &&
            accepted_session.load(std::memory_order_acquire) == frame.session &&
            router.allEndpointsOpen() &&
            (frame.session == active_session.load() ||
             frame.session == pending_session.load())) {
            active_session.store(frame.session, std::memory_order_release);
            proxy_state.store(ProxyState::Streaming, std::memory_order_release);
            // StreamStarted can be lost on the mixed UART. The right host
            // keeps retrying it until this explicit, idempotent confirmation
            // opens its endpoint-input gate.
            bridge.sendEmpty(makcm::bridge::MessageType::StreamReady,
                             frame.session);
        }
        break;
    case makcm::bridge::MessageType::DeviceGone:
        {
        const uint32_t active = active_session.load(std::memory_order_acquire);
        const uint32_t pending = pending_session.load(std::memory_order_acquire);
        const bool matches = frame.session != 0 &&
                             (frame.session == active || frame.session == pending);
        const bool idle_ack = frame.session != 0 && active == 0 &&
                              pending == 0 &&
                              !usb_installed.load(std::memory_order_acquire);
        if (matches || idle_ack) {
            // The left may have rebooted between the right-side NEW_DEV and
            // its first ProfileBegin.  It still has to acknowledge teardown;
            // otherwise the right host waits for DeviceGoneAccepted and
            // eventually resets itself on every hot-plug race.
            if (idle_ack) {
                bridge.sendEmpty(makcm::bridge::MessageType::DeviceGoneAccepted,
                                 frame.session);
                proxy_state.store(ProxyState::WaitingProfile,
                                  std::memory_order_release);
                break;
            }
            proxy_state.store(ProxyState::Disconnecting, std::memory_order_release);
            bridge.sendEmpty(makcm::bridge::MessageType::DeviceGoneAccepted,
                             frame.session);
            disconnectUsb();
            router.hotplugStick(
                static_cast<uint64_t>(esp_timer_get_time() / 1000ULL));
            active_session.store(0, std::memory_order_release);
            pending_session.store(0, std::memory_order_release);
            accepted_session.store(0, std::memory_order_release);
            profile.reset();
            dse_mask.store(0, std::memory_order_release);
            mouse_mask.store(0, std::memory_order_release);
            publishPhysicalButtons();
            proxy_state.store(ProxyState::WaitingProfile, std::memory_order_release);
        }
        }
        break;
    default:
        break;
    }
}

} // namespace

extern "C" uint8_t const *tud_descriptor_device_cb(void) {
    return profile.ready() ? profile.deviceDescriptor() : nullptr;
}

extern "C" uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    return index == 0 && profile.ready() ? profile.configurationDescriptor()
                                         : nullptr;
}

extern "C" uint8_t const *tud_descriptor_bos_cb(void) {
    return profile.ready() && profile.bosDescriptorLength() != 0
               ? profile.bosDescriptor()
               : nullptr;
}

extern "C" uint8_t const *tud_descriptor_device_qualifier_cb(void) {
    return profile.ready() && profile.qualifierDescriptorLength() != 0
               ? profile.qualifierDescriptor()
               : nullptr;
}

extern "C" uint16_t const *tud_descriptor_string_cb(uint8_t index,
                                                     uint16_t langid) {
    uint16_t length = 0;
    const uint8_t *descriptor = profile.stringDescriptor(index, langid, &length);
    if (index == 0 && descriptor == nullptr) {
        return reinterpret_cast<const uint16_t *>(fallback_langid);
    }
    if (descriptor == nullptr || length < 2) {
        // Preserve the downstream device's normal STALL semantics for an
        // unadvertised/optional string (for example MS OS index 0xEE). An
        // empty successful descriptor changes Windows' probing behavior and
        // can prevent the class driver from trying its fallback path.
        return nullptr;
    }
    return reinterpret_cast<const uint16_t *>(descriptor);
}

extern "C" void app_main(void) {
    metrics.reset();
    router.setStickRuntime(&stick_runtime);

    ota_mutex = xSemaphoreCreateMutex();
    const esp_err_t nvs_result = nvs_flash_init();
    if (ota_mutex != nullptr && nvs_result == ESP_OK) {
        if (!initPairStore()) {
            pair_record_corrupt = true;
            ota_quiesced.store(true, std::memory_order_release);
        }
        makcm::native::OtaUpdateConfig ota_config{};
        ota_config.role = makcm::ota::Target::Left;
        ota_config.protocol_version = makcm::ota::board::kPairProtocolVersion;
        ota_config.current_generation = makcm::ota::build::kPairGeneration;
        ota_config.peer_generation = makcm::ota::build::kPairGeneration;
        ota_config.partition_schema =
            makcm::ota::board::kLeftPartitionSchemaV1;
        ota_config.hardware_revision =
            makcm::ota::board::kHardwareRevisionR1;
        ota_config.project_name = makcm::ota::board::kLeftProjectName;
        // A one-generation-compatible pair migration can temporarily boot
        // the new left gateway while the old right receiver is still being
        // prepared.  Keep rollback armed, but allow enough time for the
        // relayed right-board write/reboot/health handshake before confirming
        // left last.
        ota_config.confirm_timeout_ms = 120000;
        std::memcpy(ota_config.trust.key_id,
                    makcm::ota::trust::kSignerKeyId,
                    sizeof(ota_config.trust.key_id));
        std::memcpy(ota_config.trust.public_x,
                    makcm::ota::trust::kPublicX,
                    sizeof(ota_config.trust.public_x));
        std::memcpy(ota_config.trust.public_y,
                    makcm::ota::trust::kPublicY,
                    sizeof(ota_config.trust.public_y));
        ota_config.quiesce = &otaQuiesce;
        ota_config.local_health_check = &otaLocalHealth;
        ota_config.pair_health_check = &otaPairHealth;
        ota_ready.store(ota_manager.begin(ota_config, nowMs()),
                        std::memory_order_release);
        ota_boot_prerequisites_ready.store(
            ota_ready.load(std::memory_order_acquire),
            std::memory_order_release);
        if (ota_manager.maintenanceExclusive() ||
            (pair_record_valid &&
             makcm::ota::pairRecordRequiresHostReconcile(pair_record)))
            ota_quiesced.store(true, std::memory_order_release);
    } else {
        ESP_LOGE(kTag, "OTA disabled: NVS/mutex init failed err=%s",
                 esp_err_to_name(nvs_result));
        // Loss of the persistence authority is not permission to resume the
        // ordinary proxy.  Keep USB/report handling fail-closed while the
        // CH343 control surface remains available for diagnostics/recovery.
        ota_quiesced.store(true, std::memory_order_release);
    }

    // ESP-IDF rollback confirmation is a flash/IPC operation. Execute it on
    // app_main/core 0 before any project task is created. Running it from the
    // CH343 RX task, or after UART1/USB tasks exist, can race cross-core task
    // notifications during a pending-image first boot.
    if (ota_ready.load(std::memory_order_acquire) && ota_mutex != nullptr &&
        xSemaphoreTake(ota_mutex, pdMS_TO_TICKS(250)) == pdTRUE) {
        ota_manager.poll(nowMs());
        xSemaphoreGive(ota_mutex);
    }

    // Bring up the CH343 side before the bridge RX task.  The right host emits
    // its startup/NEW_DEVICE diagnostics immediately after boot; starting the
    // bridge first used to discard those lines because bridgeText had nowhere
    // to forward them yet.  The right side waits for the USB host stack before
    // sending the binary profile, so this ordering does not lose protocol
    // frames.
    makcm::native::SecondaryConfig secondary_config;
    secondary_config.port = UART_NUM_0;
    // Arduino's ESP32-S3 Serial0 defaults to the direct UART0 IOMUX pins.
    // With CONFIG_ESP_CONSOLE_NONE, ESP-IDF does not establish that routing
    // for us, so UART_PIN_NO_CHANGE leaves the CH343 control path silent.
    secondary_config.tx_pin = 43;
    secondary_config.rx_pin = 44;
    secondary_config.initial_baud = 115200;
    secondary_config.fast_baud = 4000000;
    // secondaryText() formats bounded control/status replies on this task's
    // stack.  The 4 KiB transport default is insufficient once the 1024-byte
    // STICK response buffer enters newlib snprintf(), and the stack canary
    // reboots the board on a harmless km.caps() query.  Keep enough headroom
    // for the larger LEFT_STATUS path as well; this is control-plane memory,
    // not a per-report allocation.
    secondary_config.task_stack = 12288;
    if (!secondary.begin(secondary_config, &secondaryMove, &secondaryWheel,
                         &secondaryButton, &secondaryText,
                         &secondaryOtaFrame)) {
        ESP_LOGE(kTag, "secondary UART startup failed");
        // A data/control UART failure must not undo an image that already
        // crossed the local boot-valid boundary. Remain fail-closed and keep
        // the exact OTA/pair evidence for an explicit recovery decision.
        ota_quiesced.store(true, std::memory_order_release);
        return;
    }
    if (ota_quiesced.load(std::memory_order_acquire))
        secondary.setOtaMaintenance(true);

    makcm::native::BridgeTransportConfig bridge_config;
    bridge_config.port = UART_NUM_1;
    bridge_config.rx_pin = 1;
    bridge_config.tx_pin = 2;
    bridge_config.baud = 5000000;
    // Keep bridge backpressure separate from the per-endpoint FIFOs.  A
    // short UART burst must not make one completed endpoint report evict a
    // different endpoint's frame while the 4-IN/2-OUT profile is active.
    bridge_config.rx_buffer_size = 16384;
    bridge_config.tx_buffer_size = 8192;
    bridge_config.event_queue_size = 64;
    bridge_config.tx_queue_size = 128;
    // OTA bootstrap text lines are handled on makcm_bridge_rx and can run the
    // pair-persistence path inline: handleOtaBridgeTextLine ->
    // preparePairIntentForRequest -> mutatePairRecord -> persistPairRecord.
    // That path carries three PairTransactionRecord objects on the stack
    // (mutate's working copy, persist's by-value parameter, and its `verified`
    // readback) on top of the NVS set/commit/get internals.  The 6144 default
    // overflowed and tripped the end-of-stack watchpoint (DebugException in
    // persistPairRecord), which reads from the host as "the peer never
    // answered".  The secondary task was already raised to 12288 for the same
    // class of problem; both tasks enter this same OTA path, so both must be
    // sized for it.
    bridge_config.task_stack = 12288;
    if (!bridge.begin(bridge_config, &handleBridgeFrame, &bridgeText,
                      &bridgeOtaFrame)) {
        ESP_LOGE(kTag, "bridge UART startup failed");
        secondary.stop();
        return;
    }
    if (pair_record_valid &&
        makcm::ota::pairRecordRequiresHostReconcile(pair_record)) {
        // Any unfinished V2 record is a host-reconcile hand-off.  Restore
        // binary maintenance ownership, but do not guess a Continue/Rollback
        // action locally; Linux must query exact right-board facts first.
        right_maintenance_transaction = pair_record.transaction_id;
        ota_quiesced.store(true, std::memory_order_release);
        secondary.setOtaMaintenance(true);
        if (!bridge.setOtaMaintenance(true)) {
            ota_quiesced.store(true, std::memory_order_release);
            ESP_LOGE(kTag, "failed to restore pair-confirm UART maintenance");
        }
    }
    xTaskCreatePinnedToCore(&inputMonitorTask, "makcm_input_monitor", 2048,
                            nullptr, 4, nullptr, 1);
    ESP_LOGI(kTag, "native left proxy ready; waiting for protocol-v5 profile");
    while (true) {
        const uint32_t loop_now = nowMs();
        const uint32_t main_free_bytes = static_cast<uint32_t>(
            uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t));
        uint32_t observed_main_min =
            main_min_free_stack_bytes.load(std::memory_order_relaxed);
        while (main_free_bytes < observed_main_min &&
               !main_min_free_stack_bytes.compare_exchange_weak(
                   observed_main_min, main_free_bytes,
                   std::memory_order_release, std::memory_order_relaxed)) {
        }
        const uint32_t reset_at =
            right_link_reset_at_ms.load(std::memory_order_acquire);
        if (reset_at != 0 &&
            static_cast<int32_t>(loop_now - reset_at) >= 0 &&
            ota_mutex != nullptr &&
            xSemaphoreTake(ota_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (!tryReleaseOtaQuiesce()) {
                // Keep retrying the guarded release while an unfinished pair
                // stage or local PendingConfirm state is still present.  Use
                // a bounded retry cadence instead of logging every 10 ms.
                ESP_LOGW(kTag, "OTA release deferred: local/pair state not complete");
                right_link_reset_at_ms.store(
                    ota_quiesced.load(std::memory_order_acquire)
                        ? loop_now + 250
                        : 0,
                    std::memory_order_release);
            }
            xSemaphoreGive(ota_mutex);
        }
        if (pending_right_request_valid &&
            pending_right_request_started_ms != 0 &&
            loop_now - pending_right_request_started_ms > 7000 &&
            ota_mutex != nullptr &&
            xSemaphoreTake(ota_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            // No flash write has started while the right text bootstrap is
            // pending.  Release only that pending handshake so a later host
            // invocation can retry with a fresh transaction without a power
            // cycle or a permanent BUSY latch.
            //
            // Answer the host before clearing.  Dropping the request silently
            // made an expired bootstrap indistinguishable from a dead link: the
            // host got "timed out waiting for OTA response" and no fact at all,
            // which is the same black box as 3.6.1/3.6.4.  Timeout is the exact
            // reason, and it is the host's cue to retry with a new transaction.
            sendOtaNack(pending_right_request,
                        makcm::ota::NackReason::Timeout);
            pending_right_request_valid = false;
            pending_right_request_started_ms = 0;
            right_maintenance_transaction = 0;
            xSemaphoreGive(ota_mutex);
        }
        bool restart_left = false;
        if (ota_ready.load(std::memory_order_acquire) && ota_mutex != nullptr &&
            xSemaphoreTake(ota_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            ota_manager.poll(loop_now);
            restart_left = ota_manager.takeRestartRequest();
            xSemaphoreGive(ota_mutex);
            if (restart_left) {
                uart_wait_tx_done(UART_NUM_0, pdMS_TO_TICKS(250));
                vTaskDelay(pdMS_TO_TICKS(50));
                esp_restart();
            }
        }
        if (router.takeStickResponse(stick_status_response,
                                     sizeof(stick_status_response)))
            secondary.sendText(stick_status_response);
        const uint64_t now_ms =
            static_cast<uint64_t>(esp_timer_get_time() / 1000ULL);
        if (router.pollStickWatchdog(now_ms) &&
            usb_installed.load(std::memory_order_acquire)) {
            ESP_LOGE(kTag, "direct-stick controller stream stalled; fail-close detach");
            disconnectUsb();
            active_session.store(0, std::memory_order_release);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
