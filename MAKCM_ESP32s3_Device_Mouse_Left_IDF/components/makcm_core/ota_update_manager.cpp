#include "ota_update_manager.h"

#include "OtaBoardConfig.h"
#include "OtaGeneration.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "esp_app_format.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/ecp.h"
#include "mbedtls/private_access.h"
#include "nvs_flash.h"

namespace makcm::native {
namespace {
constexpr char kTag[] = "makcm-ota";
constexpr char kNvsNamespace[] = "makcm_ota";
constexpr char kNvsRecord[] = "local_tx";
constexpr char kNvsCompletedRecord[] = "local_done";
constexpr char kNvsSnapshotA[] = "snap_a";
constexpr char kNvsSnapshotB[] = "snap_b";

extern "C" __attribute__((used, section(".rodata.makcm_ota_identity")))
const makcm::ota::EmbeddedImageIdentity g_makcm_left_image_identity = {
    {'M', 'K', 'I', 'M', 'G', 'V', '1', '\0'},
    makcm::ota::kEmbeddedIdentityVersion,
    sizeof(makcm::ota::EmbeddedImageIdentity),
    static_cast<uint8_t>(makcm::ota::Target::Left),
    {0, 0, 0},
    MAKCM_OTA_PAIR_GENERATION,
    makcm::ota::board::kLeftPartitionSchemaV1,
    makcm::ota::board::kPairProtocolVersion,
    {0, 0},
    "makcm-s3-r1",
    "makcm_left_idf",
    MAKCM_OTA_PAIR_APP_VERSION,
    {'I', 'D', 'E', 'N', 'D', 'V', '1', '\0'},
};

bool allZero(const uint8_t *data, size_t length) {
    uint8_t aggregate = 0;
    for (size_t i = 0; i < length; ++i) aggregate |= data[i];
    return aggregate == 0;
}

bool exactField(const char *expected, const char *field, size_t capacity) {
    if (expected == nullptr || field == nullptr) return false;
    const size_t expected_length = std::strlen(expected);
    return expected_length != 0 && expected_length < capacity &&
           std::memcmp(field, expected, expected_length) == 0 &&
           field[expected_length] == '\0';
}

bool exactGenerationVersion(uint32_t generation, const char *field,
                            size_t capacity) {
    char expected[32]{};
    const int length = std::snprintf(expected, sizeof(expected), "makcm-g%lu",
                                     static_cast<unsigned long>(generation));
    return length > 0 && static_cast<size_t>(length) < sizeof(expected) &&
           exactField(expected, field, capacity);
}
} // namespace

bool OtaTrustAnchor::configured() const {
    return !allZero(key_id, sizeof(key_id)) &&
           !allZero(public_x, sizeof(public_x)) &&
           !allZero(public_y, sizeof(public_y));
}

OtaUpdateManager::~OtaUpdateManager() {
    if (!resetDownload())
        ESP_LOGE(kTag, "resetDownload failed during teardown");
    mbedtls_sha256_free(&image_hash_);
    if (nvs_ != 0) nvs_close(nvs_);
}

bool OtaUpdateManager::begin(const OtaUpdateConfig &config, uint32_t now_ms) {
    // `used` prevents compiler removal, but the Xtensa linker may still garbage
    // collect an otherwise unreferenced custom section.  A volatile load keeps
    // a real relocation from this live startup path to the embedded identity.
    const volatile uint8_t *embedded_identity =
        reinterpret_cast<const volatile uint8_t *>(
            &g_makcm_left_image_identity);
    if (embedded_identity[0] != static_cast<uint8_t>('M') ||
        embedded_identity[sizeof(g_makcm_left_image_identity) - 1] != 0 ||
        !makcm::ota::validEmbeddedImageIdentity(
            g_makcm_left_image_identity))
        return false;
    if (config.role != makcm::ota::Target::Left &&
        config.role != makcm::ota::Target::Right) return false;
    if (config.protocol_version == 0 || config.current_generation == 0 ||
        config.peer_generation == 0 || config.partition_schema == 0 ||
        config.hardware_revision == nullptr || config.project_name == nullptr ||
        config.quiesce == nullptr || config.local_health_check == nullptr ||
        config.pair_health_check == nullptr ||
        config.maintenance_timeout_ms < 1000 || config.confirm_timeout_ms < 1000)
        return false;
    config_ = config;
    mbedtls_sha256_init(&image_hash_);
    boot_evidence_.last_nvs_error =
        nvs_open(kNvsNamespace, NVS_READWRITE, &nvs_);
    if (boot_evidence_.last_nvs_error != ESP_OK) {
        ESP_LOGE(kTag, "nvs_open failed: %s (%d)",
                 esp_err_to_name(boot_evidence_.last_nvs_error),
                 static_cast<int>(boot_evidence_.last_nvs_error));
        return false;
    }
    if (!loadSnapshotV2()) {
        local_boot_validated_ = false;
        state_.store(makcm::ota::State::Fault);
        last_activity_ms_ = now_ms;
        return true;
    }

    LocalRecord record{};
    const RecordLoad record_load = loadRecord(kNvsRecord, record);
    const bool have_record = record_load == RecordLoad::Valid;
    LocalRecord completed{};
    const RecordLoad completed_load =
        loadRecord(kNvsCompletedRecord, completed);
    completed_record_valid_ =
        completed_load == RecordLoad::Valid &&
        completed.state == static_cast<uint8_t>(makcm::ota::State::Confirmed);
    if (completed_record_valid_) completed_record_ = completed;
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    boot_evidence_.reset_reason = esp_reset_reason();
    boot_evidence_.running_subtype = running == nullptr
        ? 0xFF : static_cast<uint8_t>(running->subtype);
    boot_evidence_.boot_subtype = boot == nullptr
        ? 0xFF : static_cast<uint8_t>(boot->subtype);
    boot_evidence_.running_equals_boot = running != nullptr && running == boot;
    esp_ota_img_states_t image_state = ESP_OTA_IMG_UNDEFINED;
    boot_evidence_.pre_state_error = running == nullptr
        ? ESP_ERR_NOT_FOUND
        : esp_ota_get_state_partition(running, &image_state);
    boot_evidence_.pre_state = image_state;
    boot_evidence_.post_state_error = boot_evidence_.pre_state_error;
    boot_evidence_.post_state = image_state;

    // V2 is the transaction authority.  The V1 keys below are migration and
    // downgrade aids only; they must never override a valid V2 active record
    // or completed tombstone after an interrupted multi-key legacy write.
    if (snapshot_.active.present || snapshot_.completed.present) {
        if (boot_evidence_.pre_state_error != ESP_OK ||
            !restoreFromSnapshotV2(now_ms, running, boot, image_state)) {
            local_boot_validated_ = false;
            state_.store(makcm::ota::State::Fault);
        }
        last_activity_ms_ = now_ms;
        return true;
    }
    if (record_load == RecordLoad::Invalid ||
        record_load == RecordLoad::IoError ||
        completed_load == RecordLoad::Invalid ||
        completed_load == RecordLoad::IoError ||
        (completed_load == RecordLoad::Valid && !completed_record_valid_)) {
        // A corrupt/torn/unreadable record is not equivalent to no record.
        // Never erase it or enter Idle based on an ambiguous NVS read.
        local_boot_validated_ = false;
        state_.store(makcm::ota::State::Fault);
        last_activity_ms_ = now_ms;
        return true;
    }
    // A V1 record without any V2 snapshot is the gen5-to-genN bridge.  The
    // per-state dispatch below already classifies every legal V1 case from
    // exact tx/generation/slot/image-state evidence, so there is deliberately
    // no blanket fail-closed here: intercepting the record would also hide
    // the PendingConfirm resume and the Confirmed tombstone migration.  None
    // of those paths fabricate a V2 ledger from partial V1 identity.
    const bool pending_verify =
        boot_evidence_.pre_state_error == ESP_OK &&
        image_state == ESP_OTA_IMG_PENDING_VERIFY;
    if (boot_evidence_.pre_state_error != ESP_OK) {
        // State-query failure is not proof that the rollback boundary was
        // crossed. Preserve any transaction record and fail closed.
        local_boot_validated_ = false;
        state_.store(makcm::ota::State::Fault);
        transaction_id_ = have_record ? record.transaction_id : 0;
        manifest_.generation = have_record ? record.target_generation : 0;
        last_activity_ms_ = now_ms;
        return true;
    }
    if (pending_verify) {
        local_boot_validated_ = false;
        if (!have_record ||
            record.state != static_cast<uint8_t>(
                makcm::ota::State::ActivationPending) ||
            record.target_generation != config_.current_generation ||
            record.transaction_id == 0 || running == nullptr ||
            record.partition_subtype != static_cast<uint8_t>(running->subtype)) {
            // An untraceable pending image must never mark itself valid.
            state_.store(makcm::ota::State::Fault);
            transaction_id_ = have_record ? record.transaction_id : 0;
        } else {
            state_.store(makcm::ota::State::PendingConfirm);
            transaction_id_ = record.transaction_id;
            manifest_.generation = record.target_generation;
            update_partition_ = running;
        }
        pending_confirm_started_ms_ = now_ms;
        last_activity_ms_ = now_ms;
        return true;
    }

    // Only an exact VALID state proves that the bootloader rollback boundary
    // has been crossed. Non-pending alone is insufficient evidence.
    local_boot_validated_ = image_state == ESP_OTA_IMG_VALID;

    if (have_record &&
        record.state == static_cast<uint8_t>(makcm::ota::State::PendingConfirm) &&
        record.target_generation == config_.current_generation &&
        record.transaction_id != 0 && running != nullptr &&
        record.partition_subtype == static_cast<uint8_t>(running->subtype) &&
        local_boot_validated_) {
        // mark-valid succeeded and its PendingConfirm fact was durable, but
        // power failed before pair confirmation. Resume without re-marking.
        state_.store(makcm::ota::State::PendingConfirm);
        transaction_id_ = record.transaction_id;
        manifest_.generation = record.target_generation;
        update_partition_ = running;
        pending_confirm_started_ms_ = now_ms;
    } else if (have_record && record.state ==
            static_cast<uint8_t>(makcm::ota::State::ActivationPending)) {
        // Boot selection changed but this image did not start. Preserve the
        // evidence and require maintenance instead of silently clearing it.
        state_.store(makcm::ota::State::Fault);
        transaction_id_ = record.transaction_id;
        manifest_.generation = record.target_generation;
    } else if (have_record &&
               record.state == static_cast<uint8_t>(makcm::ota::State::Confirmed) &&
               record.target_generation == config_.current_generation &&
               running != nullptr &&
               record.partition_subtype == static_cast<uint8_t>(running->subtype) &&
               local_boot_validated_) {
        // Migrate the earlier single-record completion encoding into the
        // independent tombstone. New transactions can then use local_tx
        // without erasing lost-ACK evidence.
        if (!writeRecord(kNvsCompletedRecord, record) || !clearRecord()) {
            state_.store(makcm::ota::State::Fault);
            transaction_id_ = record.transaction_id;
            manifest_.generation = record.target_generation;
        } else {
            completed_record_ = record;
            completed_record_valid_ = true;
            state_.store(makcm::ota::State::Idle);
            transaction_id_ = 0;
            manifest_.generation = 0;
        }
    } else if (!have_record) {
        state_.store(makcm::ota::State::Idle);
    } else {
        // Unknown or inconsistent durable evidence must not be erased merely
        // because the running image is outside PENDING_VERIFY.
        state_.store(makcm::ota::State::Fault);
        transaction_id_ = record.transaction_id;
        manifest_.generation = record.target_generation;
    }
    last_activity_ms_ = now_ms;
    return true;
}

bool OtaUpdateManager::maintenanceExclusive() const {
    const auto value = state_.load();
    return value != makcm::ota::State::Idle &&
           value != makcm::ota::State::Confirmed;
}

bool OtaUpdateManager::takeRestartRequest() {
    return restart_requested_.exchange(false, std::memory_order_acq_rel);
}

void OtaUpdateManager::setState(makcm::ota::State state, uint32_t now_ms) {
    state_.store(state, std::memory_order_release);
    last_activity_ms_ = now_ms;
}

uint32_t OtaUpdateManager::statusFlags() const {
    uint32_t flags = 0;
    const auto value = state_.load();
    if (value == makcm::ota::State::Prepared ||
        value == makcm::ota::State::ActivationPending)
        flags |= makcm::ota::kStatusPrepared;
    if (value == makcm::ota::State::PendingConfirm)
        flags |= makcm::ota::kStatusPendingVerify;
#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    flags |= makcm::ota::kStatusRollbackSupported;
#endif
    if (config_.trust.configured()) flags |= makcm::ota::kStatusTrustConfigured;
    if (config_.local_health_check(config_.context))
        flags |= makcm::ota::kStatusHealthPassed;
    if (local_boot_validated_)
        flags |= makcm::ota::kStatusLocalBootValidated;
    const bool peer_compatible = manifest_valid_
        ? makcm::ota::manifestCompatibleWithPeer(manifest_,
                                                  config_.peer_generation)
        : snapshot_.active.present &&
          config_.peer_generation >=
              snapshot_.active.identity.min_peer_generation &&
          config_.peer_generation <=
              snapshot_.active.identity.max_peer_generation;
    if (peer_compatible)
        flags |= makcm::ota::kStatusPeerCompatible;
    if (maintenanceExclusive()) flags |= makcm::ota::kStatusMaintenanceExclusive;
    return flags;
}

void OtaUpdateManager::fillResponse(
    const makcm::ota::Frame &request, makcm::ota::Command response_command,
    makcm::ota::NackReason reason, makcm::ota::Frame &response) const {
    // Avoid materializing a full Frame temporary on the caller task stack.
    std::memset(&response, 0, sizeof(response));
    response.target = makcm::ota::Target::Host;
    response.command = response_command;
    response.flags = makcm::ota::kFlagResponse;
    const auto current_state = state_.load();
    response.transaction_id =
        current_state != makcm::ota::State::Idle && transaction_id_ != 0
            ? transaction_id_
            : request.transaction_id;
    response.offset = next_offset_;
    response.sequence = request.sequence;
    response.length = makcm::ota::kStatusPayloadSize;
    makcm::ota::StatusPayload status{};
    status.state = current_state;
    status.last_command = request.command;
    status.reason = reason;
    status.next_offset = next_offset_;
    status.target_generation = manifest_.generation;
    status.active_generation = config_.current_generation;
    status.status_flags = statusFlags();
    makcm::ota::encodeStatusPayload(status, response.payload);
}

bool OtaUpdateManager::fillStatusV2(const makcm::ota::Frame &request,
                                    makcm::ota::Frame &response) {
    using namespace makcm::ota;
    using namespace makcm::ota::snapshot;
    if (!snapshot_ready_) return false;
    StatusPayloadV2 status{};
    status.role = config_.role;
    status.state = state_.load();
    // Status V2 is both a durable board-fact payload and the response to this
    // exact read-only STATUS request.  Keep the correlation fields tied to
    // the request/response envelope; the durable lifecycle operation remains
    // available in last_ota_operation and the active/completed records.
    status.last_command = request.command;
    status.last_response_command = Command::StatusResponse;
    status.status_flags = statusFlags();
    status.next_offset = next_offset_;
    status.active_generation = config_.current_generation;
    status.record_sequence = snapshot_.commit_sequence;
    status.record_crc32 = snapshot_crc32_;
    status.evidence_flags = kEvidenceSnapshotValid;
    if (snapshot_copy_error_)
        status.evidence_flags |= kEvidenceSnapshotCopyError;

    const Identity *identity = nullptr;
    if (snapshot_.active.present) {
        const auto &active = snapshot_.active;
        identity = &active.identity;
        status.evidence_flags |= kEvidenceActivePresent;
        status.local_phase = active.phase;
        status.reason = active.last_nack;
        status.record_transaction_id = active.identity.transaction_id;
        status.target_generation = active.identity.generation;
        status.previous_generation = active.identity.previous_generation;
        status.partition_schema = active.identity.partition_schema;
        status.image_size = active.identity.image_size;
        status.last_request_sequence = active.last_request_sequence;
        status.reset_reason = active.reset_reason;
        status.last_ota_error = active.last_ota_error;
        status.last_state_query_error = active.last_state_error;
        status.boot_attempt = active.boot_attempt;
        status.last_ota_operation = active.last_ota_operation;
        status.running_slot = active.running_slot;
        status.boot_slot = active.boot_slot;
        status.target_slot = active.identity.target_slot;
        status.previous_slot = active.identity.previous_slot;
        status.image_state = active.image_state;
        if (active.local_validated)
            status.evidence_flags |= kEvidenceLocalValidated;
        if (active.pair_committed)
            status.evidence_flags |= kEvidencePairCommitted;
    }
    if (snapshot_.completed.present) {
        const auto &completed = snapshot_.completed;
        if (identity == nullptr) identity = &completed.identity;
        status.evidence_flags |= kEvidenceCompletedPresent;
        status.completed_transaction_id =
            completed.identity.transaction_id;
        status.completed_outcome = completed.outcome;
        status.completed_terminal_command = completed.terminal_command;
        status.completed_terminal_sequence = completed.terminal_sequence;
        std::memcpy(status.completed_image_sha256,
                    completed.identity.image_sha256, 32);
        if (completed.local_validated)
            status.evidence_flags |= kEvidenceLocalValidated;
        if (completed.pair_committed)
            status.evidence_flags |= kEvidencePairCommitted;
    }
    if (identity != nullptr) {
        status.protocol_epoch = identity->protocol_epoch;
        if (!snapshot_.active.present) {
            status.target_generation = identity->generation;
            status.previous_generation = identity->previous_generation;
            status.partition_schema = identity->partition_schema;
            status.image_size = identity->image_size;
            status.target_slot = identity->target_slot;
            status.previous_slot = identity->previous_slot;
        }
        std::memcpy(status.image_sha256, identity->image_sha256, 32);
        std::memcpy(status.manifest_sha256, identity->manifest_sha256, 32);
        std::memcpy(status.identity_digest, identity->identity_digest, 32);
        std::memcpy(status.previous_image_sha256,
                    identity->previous_image_sha256, 32);
        std::memcpy(status.hardware_revision, identity->hardware_revision,
                    sizeof(status.hardware_revision));
        std::memcpy(status.project_name, identity->project_name,
                    sizeof(status.project_name));
        if (identityDigestMatches(*identity))
            status.evidence_flags |= kEvidenceIdentityDigestValid;
    } else {
        std::snprintf(status.hardware_revision,
                      sizeof(status.hardware_revision), "%s",
                      config_.hardware_revision);
        std::snprintf(status.project_name, sizeof(status.project_name), "%s",
                      config_.project_name);
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    status.running_slot = running == nullptr
        ? 0xFF : static_cast<uint8_t>(running->subtype);
    status.boot_slot = boot == nullptr
        ? 0xFF : static_cast<uint8_t>(boot->subtype);
    esp_ota_img_states_t image_state = ESP_OTA_IMG_UNDEFINED;
    const esp_err_t state_error = running == nullptr
        ? ESP_ERR_NOT_FOUND
        : esp_ota_get_state_partition(running, &image_state);
    status.last_state_query_error = static_cast<int32_t>(state_error);
    status.image_state = state_error == ESP_OK
        ? static_cast<uint8_t>(image_state)
        : 0xFF;
    if (running != nullptr &&
        esp_partition_get_sha256(running, status.running_image_sha256) ==
            ESP_OK)
        status.evidence_flags |= kEvidenceRunningImageHashValid;
    esp_app_desc_t descriptor{};
    if (running != nullptr &&
        esp_ota_get_partition_description(running, &descriptor) == ESP_OK) {
        std::memcpy(status.app_elf_sha256, descriptor.app_elf_sha256,
                    sizeof(status.app_elf_sha256));
        std::memcpy(status.app_version, descriptor.version,
                    sizeof(status.app_version));
    }

    std::memset(&response, 0, sizeof(response));
    response.target = Target::Host;
    response.command = Command::StatusResponse;
    response.flags = kFlagResponse | kFlagStatusV2;
    response.transaction_id = request.transaction_id;
    response.offset = next_offset_;
    response.sequence = request.sequence;
    if (!encodeStatusV2(status, response.payload, sizeof(response.payload)))
        return false;
    response.length = kStatusV2PayloadSize;
    return true;
}

bool OtaUpdateManager::handle(const makcm::ota::Frame &request,
                              makcm::ota::Frame &response, uint32_t now_ms) {
    makcm::ota::NackReason reason = makcm::ota::NackReason::None;
    bool ok = false;
    const bool state_changing =
        makcm::ota::snapshot::ledgerIndex(request.command) <
        makcm::ota::snapshot::kLedgerCount;
    const bool empty_maintenance_abort =
        request.target == config_.role &&
        request.command == makcm::ota::Command::Abort &&
        state_.load() == makcm::ota::State::Maintenance &&
        !snapshot_.active.present;
    if (request.target == config_.role && state_changing) {
        const ReplayDecision replay = replayCommand(request, response);
        if (replay == ReplayDecision::Replayed) {
            last_activity_ms_ = now_ms;
            return response.command == makcm::ota::Command::Ack;
        }
        if (replay == ReplayDecision::ResumeIntent ||
            replay == ReplayDecision::ResumeObserved) {
            const bool resumed = resumeIncompleteCommand(
                request, replay, response, now_ms, reason);
            if (!resumed && response.length == 0)
                fillResponse(request, makcm::ota::Command::Nack, reason,
                             response);
            return resumed;
        }
        if (replay == ReplayDecision::Conflict) {
            reason = makcm::ota::NackReason::WrongTransaction;
            fillResponse(request, makcm::ota::Command::Nack, reason, response);
            return false;
        }
    }
    if (request.target != config_.role) {
        reason = makcm::ota::NackReason::WrongTarget;
    } else if (request.command == makcm::ota::Command::Hello ||
               request.command == makcm::ota::Command::Status) {
        ok = true;
        last_activity_ms_ = now_ms;
    } else if (request.command == makcm::ota::Command::EnterMaintenance) {
        ok = enter(request, now_ms, reason);
    } else if (request.command == makcm::ota::Command::Abort &&
               request.transaction_id != 0 &&
               state_.load() == makcm::ota::State::Fault &&
               !snapshot_.active.present) {
        // The one recovery entry point.  A Fault holding no durable identity
        // cannot be cleared any other way: ENTER is refused as InvalidState,
        // and the guard below answers WrongTransaction to everything else --
        // including the ABORT that is supposed to clear the Fault.  Physical
        // NVS erasure was the only way out.
        //
        // The match is deliberately against the durable record, not the
        // volatile transaction_id_.  A board that faulted after ENTER but
        // before any manifest still holds a volatile id that was never
        // published in Status V2, so the host cannot name it and keying on it
        // would leave exactly that board unreachable.  Adopting the request's
        // id is safe precisely because active.present is false: there is no
        // evidence to overwrite, and a Fault that does hold durable evidence
        // still requires the exact transaction and keeps it.
        transaction_id_ = request.transaction_id;
        ok = abort(request, true, reason);
        if (!ok) transaction_id_ = 0;
    } else if (request.transaction_id == 0 ||
               request.transaction_id != transaction_id_) {
        reason = makcm::ota::NackReason::WrongTransaction;
    } else {
        switch (request.command) {
        case makcm::ota::Command::Manifest:
            ok = acceptManifest(request, now_ms, reason);
            break;
        case makcm::ota::Command::Data:
            ok = acceptData(request, now_ms, reason);
            break;
        case makcm::ota::Command::Prepare:
            ok = prepare(request, now_ms, reason);
            break;
        case makcm::ota::Command::Activate:
            ok = activate(request, now_ms, reason);
            break;
        case makcm::ota::Command::Confirm:
            ok = confirm(request, now_ms, reason);
            break;
        case makcm::ota::Command::Abort:
            ok = abort(request, true, reason);
            break;
        default:
            reason = makcm::ota::NackReason::InvalidState;
            break;
        }
    }
    const auto response_command = request.command == makcm::ota::Command::Status && ok
        ? makcm::ota::Command::StatusResponse
        : (ok ? makcm::ota::Command::Ack : makcm::ota::Command::Nack);
    if (!(request.command == makcm::ota::Command::Status && ok &&
          (request.flags & makcm::ota::kFlagStatusV2) != 0 &&
          fillStatusV2(request, response)))
        fillResponse(request, response_command, reason, response);
    // ENTER_MAINTENANCE deliberately has no persistent transaction identity
    // until a signed manifest is accepted.  Its matching empty ABORT is a
    // session cancellation, so there is no valid V2 ledger in which to
    // finalize it.  All image-affecting ABORT paths still require durable
    // intent/fact records below.
    if (state_changing && !(empty_maintenance_abort && ok) &&
        !finalizeCommandResponse(request, response, reason, ok)) {
        ok = false;
        reason = makcm::ota::NackReason::FlashWriteFailed;
        fillResponse(request, makcm::ota::Command::Nack, reason, response);
    }
    return ok;
}

bool OtaUpdateManager::enter(const makcm::ota::Frame &request, uint32_t now_ms,
                             makcm::ota::NackReason &reason) {
    const auto value = state_.load();
    if (request.transaction_id == 0 || request.length != 0) {
        reason = makcm::ota::NackReason::Malformed;
        return false;
    }
    if (value == makcm::ota::State::PendingConfirm ||
        value == makcm::ota::State::Fault) {
        reason = makcm::ota::NackReason::InvalidState;
        return false;
    }
    if (value != makcm::ota::State::Idle &&
        transaction_id_ != request.transaction_id) {
        reason = makcm::ota::NackReason::Busy;
        return false;
    }
    if (value == makcm::ota::State::Idle && !config_.quiesce(config_.context)) {
        reason = makcm::ota::NackReason::Busy;
        return false;
    }
    if (!resetDownload()) {
        reason = makcm::ota::NackReason::FlashWriteFailed;
        setState(makcm::ota::State::Fault, now_ms);
        return false;
    }
    transaction_id_ = request.transaction_id;
    setState(makcm::ota::State::Maintenance, now_ms);
    // The target generation is not known until a signed manifest arrives;
    // persisting a generation-zero transaction would be ambiguous at boot.
    return true;
}

bool OtaUpdateManager::verifyManifestSignature(const uint8_t *wire) const {
    if (wire == nullptr || !config_.trust.configured() ||
        std::memcmp(wire + 112, config_.trust.key_id,
                    sizeof(config_.trust.key_id)) != 0) return false;
    uint8_t digest[32]{};
    if (mbedtls_sha256(wire, makcm::ota::kManifestSignedSize, digest, 0) != 0)
        return false;

    mbedtls_ecdsa_context context;
    mbedtls_mpi r;
    mbedtls_mpi s;
    mbedtls_ecdsa_init(&context);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);
    bool ok = mbedtls_ecp_group_load(
                  &context.MBEDTLS_PRIVATE(grp), MBEDTLS_ECP_DP_SECP256R1) == 0;
    ok = ok && mbedtls_mpi_read_binary(
                   &context.MBEDTLS_PRIVATE(Q).MBEDTLS_PRIVATE(X),
                   config_.trust.public_x, sizeof(config_.trust.public_x)) == 0;
    ok = ok && mbedtls_mpi_read_binary(
                   &context.MBEDTLS_PRIVATE(Q).MBEDTLS_PRIVATE(Y),
                   config_.trust.public_y, sizeof(config_.trust.public_y)) == 0;
    ok = ok && mbedtls_mpi_lset(
                   &context.MBEDTLS_PRIVATE(Q).MBEDTLS_PRIVATE(Z), 1) == 0;
    ok = ok && mbedtls_mpi_read_binary(
                   &r, wire + makcm::ota::kManifestSignedSize, 32) == 0;
    ok = ok && mbedtls_mpi_read_binary(
                   &s, wire + makcm::ota::kManifestSignedSize + 32, 32) == 0;
    ok = ok && mbedtls_ecdsa_verify(
                   &context.MBEDTLS_PRIVATE(grp), digest, sizeof(digest),
                   &context.MBEDTLS_PRIVATE(Q), &r, &s) == 0;
    mbedtls_mpi_free(&s);
    mbedtls_mpi_free(&r);
    mbedtls_ecdsa_free(&context);
    return ok;
}

bool OtaUpdateManager::acceptManifest(
    const makcm::ota::Frame &request, uint32_t now_ms,
    makcm::ota::NackReason &reason) {
    if (state_.load() != makcm::ota::State::Maintenance ||
        request.offset != 0 || request.length != makcm::ota::kManifestWireSize) {
        reason = makcm::ota::NackReason::InvalidState;
        return false;
    }
    makcm::ota::Manifest manifest{};
    if (!makcm::ota::decodeManifest(request.payload, request.length, manifest) ||
        manifest.role != config_.role ||
        manifest.protocol_version != config_.protocol_version ||
        manifest.partition_schema != config_.partition_schema ||
        manifest.generation <= config_.current_generation ||
        !makcm::ota::manifestCompatibleWithPeer(manifest,
                                                config_.peer_generation) ||
        !exactField(config_.hardware_revision, manifest.hardware_revision,
                    sizeof(manifest.hardware_revision)) ||
        !exactField(config_.project_name, manifest.project_name,
                    sizeof(manifest.project_name))) {
        reason = makcm::ota::NackReason::CompatibilityRejected;
        return false;
    }
    if (!verifyManifestSignature(request.payload)) {
        reason = makcm::ota::NackReason::SignatureVerificationFailed;
        return false;
    }
    update_partition_ = esp_ota_get_next_update_partition(nullptr);
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_app_desc_t running_descriptor{};
    if (update_partition_ == nullptr || update_partition_ == running ||
        update_partition_->type != ESP_PARTITION_TYPE_APP ||
        manifest.image_size > update_partition_->size || running == nullptr ||
        esp_ota_get_partition_description(running, &running_descriptor) !=
            ESP_OK ||
        !exactField(config_.project_name, running_descriptor.project_name,
                    sizeof(running_descriptor.project_name)) ||
        !exactGenerationVersion(config_.current_generation,
                                running_descriptor.version,
                                sizeof(running_descriptor.version))) {
        reason = makcm::ota::NackReason::ManifestRejected;
        return false;
    }
    manifest_ = manifest;
    std::memcpy(manifest_wire_, request.payload, sizeof(manifest_wire_));
    manifest_valid_ = true;
    next_offset_ = 0;
    last_data_length_ = 0;

    // MANIFEST becomes the active identity before esp_ota_begin can erase or
    // allocate any target-slot state. ACK is impossible until both the intent
    // and the observed result have survived A/B readback.
    auto &next_snapshot = beginSnapshotMutation();
    if (!buildActiveIdentity(request) ||
        !setLedgerIntent(next_snapshot.active, request,
                         makcm::ota::State::Maintenance,
                         makcm::ota::snapshot::Phase::Receiving) ||
        !commitPendingSnapshotV2()) {
        reason = makcm::ota::NackReason::FlashWriteFailed;
        return false;
    }
    boot_evidence_.last_ota_error =
        esp_ota_begin(update_partition_, manifest.image_size, &ota_handle_);
    if (boot_evidence_.last_ota_error != ESP_OK) {
        ESP_LOGE(kTag, "esp_ota_begin failed: %s (%d)",
                 esp_err_to_name(boot_evidence_.last_ota_error),
                 static_cast<int>(boot_evidence_.last_ota_error));
        reason = makcm::ota::NackReason::FlashWriteFailed;
        setState(makcm::ota::State::Fault, now_ms);
        if (!persistCommandFact(
                request,
                makcm::ota::snapshot::EffectPhase::ExternalEffectObserved,
                makcm::ota::snapshot::Phase::Failed, reason,
                makcm::ota::State::Fault))
            reason = makcm::ota::NackReason::FlashWriteFailed;
        return false;
    }
    ota_open_ = true;
    if (mbedtls_sha256_starts(&image_hash_, 0) != 0) {
        const bool abort_ok = resetDownload();
        reason = abort_ok
            ? makcm::ota::NackReason::ImageVerificationFailed
            : makcm::ota::NackReason::FlashWriteFailed;
        setState(makcm::ota::State::Fault, now_ms);
        if (!persistCommandFact(
                request,
                makcm::ota::snapshot::EffectPhase::ExternalEffectObserved,
                makcm::ota::snapshot::Phase::Failed, reason,
                makcm::ota::State::Fault))
            reason = makcm::ota::NackReason::FlashWriteFailed;
        return false;
    }
    hash_open_ = true;
    setState(makcm::ota::State::Receiving, now_ms);
    if (!persistCommandFact(
            request,
            makcm::ota::snapshot::EffectPhase::ExternalEffectObserved,
            makcm::ota::snapshot::Phase::Receiving,
            makcm::ota::NackReason::None,
            makcm::ota::State::Receiving)) {
        reason = makcm::ota::NackReason::FlashWriteFailed;
        return false;
    }
    if (!saveRecord())
        ESP_LOGW(kTag, "legacy receiving mirror write failed");
    return true;
}

bool OtaUpdateManager::validateFirstChunk(const uint8_t *data,
                                          size_t length) const {
    const size_t descriptor_offset = sizeof(esp_image_header_t) +
                                     sizeof(esp_image_segment_header_t);
    if (data == nullptr || length < descriptor_offset + sizeof(esp_app_desc_t) ||
        data[0] != ESP_IMAGE_HEADER_MAGIC) return false;
    esp_app_desc_t descriptor{};
    std::memcpy(&descriptor, data + descriptor_offset, sizeof(descriptor));
    return descriptor.magic_word == ESP_APP_DESC_MAGIC_WORD &&
           exactField(config_.project_name, descriptor.project_name,
                      sizeof(descriptor.project_name));
}

bool OtaUpdateManager::validateEmbeddedIdentity(
    const esp_partition_t *partition, uint32_t image_size,
    uint32_t generation) const {
    if (partition == nullptr || image_size <
            sizeof(makcm::ota::EmbeddedImageIdentity))
        return false;
    const void *mapped = nullptr;
    esp_partition_mmap_handle_t handle = 0;
    if (esp_partition_mmap(partition, 0, image_size,
                           ESP_PARTITION_MMAP_DATA, &mapped, &handle) !=
            ESP_OK || mapped == nullptr)
        return false;
    size_t match_count = 0;
    const auto *bytes = static_cast<const uint8_t *>(mapped);
    for (size_t offset = 0;
         offset + sizeof(makcm::ota::EmbeddedImageIdentity) <= image_size;
         ++offset) {
        if (std::memcmp(bytes + offset, makcm::ota::kEmbeddedIdentityMagic,
                        sizeof(makcm::ota::kEmbeddedIdentityMagic)) != 0)
            continue;
        makcm::ota::EmbeddedImageIdentity identity{};
        std::memcpy(&identity, bytes + offset, sizeof(identity));
        char expected_version[32]{};
        const int length = std::snprintf(
            expected_version, sizeof(expected_version), "makcm-g%lu",
            static_cast<unsigned long>(generation));
        const bool matched =
            makcm::ota::validEmbeddedImageIdentity(identity) &&
            identity.role == static_cast<uint8_t>(config_.role) &&
            identity.generation == generation &&
            identity.partition_schema == config_.partition_schema &&
            identity.protocol_version == config_.protocol_version &&
            exactField(config_.hardware_revision,
                       identity.hardware_revision,
                       sizeof(identity.hardware_revision)) &&
            exactField(config_.project_name, identity.project_name,
                       sizeof(identity.project_name)) &&
            length > 0 && static_cast<size_t>(length) <
                              sizeof(expected_version) &&
            exactField(expected_version, identity.app_version,
                       sizeof(identity.app_version));
        if (matched && ++match_count > 1) break;
    }
    esp_partition_munmap(handle);
    return match_count == 1;
}

bool OtaUpdateManager::acceptData(const makcm::ota::Frame &request,
                                  uint32_t now_ms,
                                  makcm::ota::NackReason &reason) {
    if (state_.load() != makcm::ota::State::Receiving || !ota_open_ ||
        !hash_open_ || !manifest_valid_ || request.length == 0 ||
        request.length > manifest_.chunk_size ||
        static_cast<uint64_t>(request.offset) + request.length >
            manifest_.image_size) {
        reason = makcm::ota::NackReason::InvalidState;
        return false;
    }
    const uint32_t data_crc = makcm::ota::crc32(request.payload, request.length);
    uint8_t data_sha256[32]{};
    if (mbedtls_sha256(request.payload, request.length,
                       data_sha256, 0) != 0) {
        reason = makcm::ota::NackReason::ImageVerificationFailed;
        return false;
    }
    if (request.offset == last_data_offset_ && request.length == last_data_length_ &&
        request.sequence == last_data_sequence_ && data_crc == last_data_crc_ &&
        std::memcmp(data_sha256, last_data_sha256_,
                    sizeof(data_sha256)) == 0) {
        // Exact replay of the last ACKed block is idempotent.
        last_activity_ms_ = now_ms;
        return true;
    }
    if (request.offset != next_offset_) {
        reason = makcm::ota::NackReason::WrongOffset;
        return false;
    }
    if (next_offset_ == 0 && !validateFirstChunk(request.payload, request.length)) {
        reason = makcm::ota::NackReason::ManifestRejected;
        return false;
    }
    boot_evidence_.last_ota_error =
        esp_ota_write(ota_handle_, request.payload, request.length);
    if (boot_evidence_.last_ota_error != ESP_OK ||
        mbedtls_sha256_update(&image_hash_, request.payload, request.length) != 0) {
        ESP_LOGE(kTag, "esp_ota_write/hash failed: %s (%d)",
                 esp_err_to_name(boot_evidence_.last_ota_error),
                 static_cast<int>(boot_evidence_.last_ota_error));
        reason = makcm::ota::NackReason::FlashWriteFailed;
        return false;
    }
    last_data_offset_ = request.offset;
    last_data_length_ = request.length;
    last_data_crc_ = data_crc;
    last_data_sequence_ = request.sequence;
    std::memcpy(last_data_sha256_, data_sha256,
                sizeof(last_data_sha256_));
    next_offset_ += request.length;
    last_activity_ms_ = now_ms;
    return true;
}

bool OtaUpdateManager::prepare(const makcm::ota::Frame &request,
                               uint32_t now_ms,
                               makcm::ota::NackReason &reason) {
    if (state_.load() != makcm::ota::State::Receiving || !ota_open_ ||
        next_offset_ != manifest_.image_size) {
        reason = makcm::ota::NackReason::InvalidState;
        return false;
    }
    uint8_t digest[32]{};
    if (mbedtls_sha256_finish(&image_hash_, digest) != 0) {
        reason = makcm::ota::NackReason::ImageVerificationFailed;
        return false;
    }
    hash_open_ = false;
    if (std::memcmp(digest, manifest_.image_sha256, sizeof(digest)) != 0) {
        const bool abort_ok = resetDownload();
        setState(makcm::ota::State::Fault, now_ms);
        reason = abort_ok
            ? makcm::ota::NackReason::ImageVerificationFailed
            : makcm::ota::NackReason::FlashWriteFailed;
        return false;
    }
    if (!persistCommandIntent(
            request, makcm::ota::State::Receiving,
            makcm::ota::snapshot::Phase::Receiving)) {
        reason = makcm::ota::NackReason::FlashWriteFailed;
        return false;
    }
    const esp_err_t end_result = esp_ota_end(ota_handle_);
    boot_evidence_.last_ota_error = end_result;
    ota_open_ = false;
    ota_handle_ = 0;
    esp_app_desc_t descriptor{};
    const esp_err_t descriptor_result = update_partition_ == nullptr
        ? ESP_ERR_NOT_FOUND
        : esp_ota_get_partition_description(update_partition_, &descriptor);
    if (end_result != ESP_OK || descriptor_result != ESP_OK ||
        !exactField(config_.project_name, descriptor.project_name,
                    sizeof(descriptor.project_name)) ||
        !exactGenerationVersion(manifest_.generation, descriptor.version,
                                sizeof(descriptor.version)) ||
        !validateEmbeddedIdentity(update_partition_, manifest_.image_size,
                                  manifest_.generation)) {
        ESP_LOGE(kTag, "prepare verification failed end=%s (%d) desc=%s (%d)",
                 esp_err_to_name(end_result), static_cast<int>(end_result),
                 esp_err_to_name(descriptor_result),
                 static_cast<int>(descriptor_result));
        setState(makcm::ota::State::Fault, now_ms);
        reason = makcm::ota::NackReason::ImageVerificationFailed;
        if (!persistCommandFact(
                request,
                makcm::ota::snapshot::EffectPhase::ExternalEffectObserved,
                makcm::ota::snapshot::Phase::Failed, reason,
                makcm::ota::State::Fault))
            reason = makcm::ota::NackReason::FlashWriteFailed;
        return false;
    }
    setState(makcm::ota::State::Prepared, now_ms);
    if (!persistCommandFact(
            request,
            makcm::ota::snapshot::EffectPhase::ExternalEffectObserved,
            makcm::ota::snapshot::Phase::Prepared,
            makcm::ota::NackReason::None,
            makcm::ota::State::Prepared)) {
        reason = makcm::ota::NackReason::FlashWriteFailed;
        return false;
    }
    if (!saveRecord())
        ESP_LOGW(kTag, "legacy prepared mirror write failed");
    return true;
}

bool OtaUpdateManager::activate(const makcm::ota::Frame &request,
                                uint32_t now_ms,
                                makcm::ota::NackReason &reason) {
    if (state_.load() != makcm::ota::State::Prepared ||
        update_partition_ == nullptr) {
        reason = makcm::ota::NackReason::InvalidState;
        return false;
    }
#if !CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    reason = makcm::ota::NackReason::RollbackUnavailable;
    return false;
#else
    if (!persistCommandIntent(
            request, makcm::ota::State::Prepared,
            makcm::ota::snapshot::Phase::ActivateIntent)) {
        reason = makcm::ota::NackReason::FlashWriteFailed;
        return false;
    }
    // Persist intent before changing boot selection. An interrupted command is
    // then recoverable by comparing the record with the boot partition.
    setState(makcm::ota::State::ActivationPending, now_ms);
    if (!saveRecord())
        ESP_LOGW(kTag, "legacy activation mirror write failed");
    boot_evidence_.last_ota_error =
        esp_ota_set_boot_partition(update_partition_);
    if (boot_evidence_.last_ota_error != ESP_OK) {
        ESP_LOGE(kTag, "esp_ota_set_boot_partition failed: %s (%d)",
                 esp_err_to_name(boot_evidence_.last_ota_error),
                 static_cast<int>(boot_evidence_.last_ota_error));
        reason = makcm::ota::NackReason::FlashWriteFailed;
        if (!persistCommandFact(
                request,
                makcm::ota::snapshot::EffectPhase::ExternalEffectObserved,
                makcm::ota::snapshot::Phase::ActivateIntent, reason,
                makcm::ota::State::ActivationPending))
            reason = makcm::ota::NackReason::FlashWriteFailed;
        return false;
    }
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    boot_evidence_.boot_subtype = boot == nullptr
        ? 0xFF : static_cast<uint8_t>(boot->subtype);
    if (boot != update_partition_) {
        boot_evidence_.last_ota_error = ESP_ERR_INVALID_STATE;
        reason = makcm::ota::NackReason::FlashWriteFailed;
        if (!persistCommandFact(
                request,
                makcm::ota::snapshot::EffectPhase::ExternalEffectObserved,
                makcm::ota::snapshot::Phase::ActivateIntent, reason,
                makcm::ota::State::ActivationPending))
            reason = makcm::ota::NackReason::FlashWriteFailed;
        return false;
    }
    if (!persistCommandFact(
            request,
            makcm::ota::snapshot::EffectPhase::ExternalEffectObserved,
            makcm::ota::snapshot::Phase::BootSelected,
            makcm::ota::NackReason::None,
            makcm::ota::State::ActivationPending)) {
        reason = makcm::ota::NackReason::FlashWriteFailed;
        return false;
    }
    restart_requested_.store(true, std::memory_order_release);
    return true;
#endif
}

bool OtaUpdateManager::confirm(const makcm::ota::Frame &request,
                               uint32_t now_ms,
                               makcm::ota::NackReason &reason) {
    if (state_.load() != makcm::ota::State::PendingConfirm ||
        !local_boot_validated_ ||
        !config_.pair_health_check(config_.context)) {
        reason = makcm::ota::NackReason::CompatibilityRejected;
        return false;
    }
#if !CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    reason = makcm::ota::NackReason::RollbackUnavailable;
    return false;
#else
    if (!persistCommandIntent(
            request, makcm::ota::State::PendingConfirm,
            makcm::ota::snapshot::Phase::PairPending)) {
        reason = makcm::ota::NackReason::FlashWriteFailed;
        return false;
    }
    setState(makcm::ota::State::Confirmed, now_ms);
    if (!saveCompletedRecord())
        ESP_LOGW(kTag, "legacy completion mirror write failed");
    if (!clearRecord())
        ESP_LOGW(kTag, "legacy active mirror clear failed");
    // Confirmation ACK is emitted before the main loop consumes this flag.
    // Reboot into a clean runtime so quiesced USB/UART gates are not left
    // latched after the pair transaction has been durably confirmed.
    restart_requested_.store(true, std::memory_order_release);
    return true;
#endif
}

bool OtaUpdateManager::validateLocalBoot(uint32_t now_ms) {
    if (local_boot_validated_) return true;
    if (state_.load() != makcm::ota::State::PendingConfirm ||
        !config_.local_health_check(config_.context)) {
        return false;
    }
#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    if (running == nullptr || boot == nullptr || running != boot) {
        setState(makcm::ota::State::Fault, now_ms);
        return false;
    }

    // Prefer V2 identity.  A gen5→gen15 first boot may only have the V1
    // ActivationPending/PendingConfirm record: still allow mark-valid when
    // that record exactly matches the running PENDING_VERIFY slot.
    const bool v2_active = snapshot_.active.present;
    if (v2_active) {
        if (snapshot_.active.identity.transaction_id != transaction_id_ ||
            snapshot_.active.identity.generation !=
                config_.current_generation ||
            static_cast<uint8_t>(running->subtype) !=
                snapshot_.active.identity.target_slot ||
            !identityDigestMatches(snapshot_.active.identity) ||
            !partitionMatchesImage(running, snapshot_.active.identity)) {
            setState(makcm::ota::State::Fault, now_ms);
            return false;
        }
    } else {
        LocalRecord record{};
        if (loadRecord(kNvsRecord, record) != RecordLoad::Valid ||
            record.transaction_id == 0 ||
            record.transaction_id != transaction_id_ ||
            record.target_generation != config_.current_generation ||
            record.partition_subtype !=
                static_cast<uint8_t>(running->subtype) ||
            (record.state != static_cast<uint8_t>(
                 makcm::ota::State::PendingConfirm) &&
             record.state != static_cast<uint8_t>(
                 makcm::ota::State::ActivationPending))) {
            setState(makcm::ota::State::Fault, now_ms);
            return false;
        }
    }

    esp_app_desc_t descriptor{};
    if (esp_ota_get_partition_description(running, &descriptor) != ESP_OK ||
        !exactField(config_.project_name, descriptor.project_name,
                    sizeof(descriptor.project_name))) {
        setState(makcm::ota::State::Fault, now_ms);
        return false;
    }

    esp_ota_img_states_t pre_state = ESP_OTA_IMG_UNDEFINED;
    boot_evidence_.pre_state_error =
        esp_ota_get_state_partition(running, &pre_state);
    boot_evidence_.pre_state = pre_state;
    if (boot_evidence_.pre_state_error != ESP_OK ||
        pre_state != ESP_OTA_IMG_PENDING_VERIFY) return false;

    boot_evidence_.mark_valid_error =
        esp_ota_mark_app_valid_cancel_rollback();
    if (boot_evidence_.mark_valid_error != ESP_OK) {
        ESP_LOGE(kTag, "mark-valid failed: %s (%d)",
                 esp_err_to_name(boot_evidence_.mark_valid_error),
                 static_cast<int>(boot_evidence_.mark_valid_error));
        return false;
    }

    esp_ota_img_states_t post_state = ESP_OTA_IMG_UNDEFINED;
    boot_evidence_.post_state_error =
        esp_ota_get_state_partition(running, &post_state);
    boot_evidence_.post_state = post_state;
    if (boot_evidence_.post_state_error != ESP_OK ||
        post_state != ESP_OTA_IMG_VALID) {
        ESP_LOGE(kTag, "mark-valid readback failed: %s (%d), state=%d",
                 esp_err_to_name(boot_evidence_.post_state_error),
                 static_cast<int>(boot_evidence_.post_state_error),
                 static_cast<int>(post_state));
        return false;
    }
    local_boot_validated_ = true;
    last_activity_ms_ = now_ms;
    // Persist the local-validation fact immediately. If power fails before
    // pair confirmation, begin() can resume PendingConfirm without calling
    // mark-valid a second time or losing the transaction identity.
    if (v2_active && !persistLocalValidationFact()) {
        setState(makcm::ota::State::Fault, now_ms);
        return false;
    }
    // V1 is retained for the legacy bridge and downgrade recovery.  When V2
    // is present it remains authoritative; a legacy mirror failure must not
    // erase a successfully committed V2 fact.
    if (!saveRecord())
        ESP_LOGW(kTag, "legacy local-validation mirror write failed");
    return true;
#else
    return false;
#endif
}

bool OtaUpdateManager::abort(const makcm::ota::Frame &request,
                             bool request_restart,
                             makcm::ota::NackReason &reason) {
    const auto abort_state = state_.load();
    if ((abort_state == makcm::ota::State::Maintenance ||
         abort_state == makcm::ota::State::Fault) &&
        !snapshot_.active.present) {
        // No manifest was accepted, no OTA partition was opened, and no boot
        // selector can have changed.  Cancel this transient maintenance
        // session without manufacturing a generation-zero durable record.
        //
        // Fault belongs here for exactly the same reason.  A board that
        // faulted while holding no active transaction had no way back at all:
        // ENTER is refused from Fault as InvalidState, and every durable path
        // below needs an active identity it does not have, so each one reports
        // FlashWriteFailed.  The only remaining recovery was erasing NVS over a
        // physical BOOT-mode cable -- on the one board whose whole purpose is
        // to be updatable from the trusted host.  There is no evidence to
        // preserve here: active.present is false by construction.
        if (!resetDownload()) {
            reason = makcm::ota::NackReason::FlashWriteFailed;
            setState(makcm::ota::State::Fault, last_activity_ms_);
            return false;
        }
        transaction_id_ = 0;
        manifest_ = {};
        setState(makcm::ota::State::Idle, last_activity_ms_);
        restart_requested_.store(false, std::memory_order_release);
        return true;
    }
    if (state_.load() == makcm::ota::State::ActivationPending) {
        // set_boot may already have changed the boot selector. Clearing the
        // intent here would make the next PENDING_VERIFY boot untraceable.
        reason = makcm::ota::NackReason::RollbackUnavailable;
        return false;
    }
    if (state_.load() == makcm::ota::State::PendingConfirm) {
#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
        restart_requested_.store(false, std::memory_order_release);
        // Do not clear evidence before rollback completion. V1 does not retain
        // a trustworthy previous-slot identity after local validation, so do
        // not guess a write-target API result as the historical slot.
        if (local_boot_validated_) {
            reason = makcm::ota::NackReason::RollbackUnavailable;
            return false;
        }
        if (!persistCommandIntent(
                request, makcm::ota::State::PendingConfirm,
                makcm::ota::snapshot::Phase::RollbackIntent)) {
            reason = makcm::ota::NackReason::FlashWriteFailed;
            return false;
        }
        boot_evidence_.last_ota_error =
            esp_ota_mark_app_invalid_rollback_and_reboot();
        ESP_LOGE(kTag, "rollback returned: %s (%d)",
                 esp_err_to_name(boot_evidence_.last_ota_error),
                 static_cast<int>(boot_evidence_.last_ota_error));
        reason = makcm::ota::NackReason::FlashWriteFailed;
        if (!persistCommandFact(
                request,
                makcm::ota::snapshot::EffectPhase::ExternalEffectObserved,
                makcm::ota::snapshot::Phase::Failed, reason,
                makcm::ota::State::Fault))
            reason = makcm::ota::NackReason::FlashWriteFailed;
        return false;
#else
        reason = makcm::ota::NackReason::RollbackUnavailable;
        return false;
#endif
    }
    if (!persistCommandIntent(
            request, state_.load(),
            makcm::ota::snapshot::Phase::RollbackIntent)) {
        reason = makcm::ota::NackReason::FlashWriteFailed;
        return false;
    }
    if (!resetDownload()) {
        reason = makcm::ota::NackReason::FlashWriteFailed;
        setState(makcm::ota::State::Fault, last_activity_ms_);
        if (!persistCommandFact(
                request,
                makcm::ota::snapshot::EffectPhase::ExternalEffectObserved,
                makcm::ota::snapshot::Phase::Failed, reason,
                makcm::ota::State::Fault))
            reason = makcm::ota::NackReason::FlashWriteFailed;
        return false;
    }
    if (!clearRecord())
        ESP_LOGW(kTag, "legacy abort mirror clear failed");
    transaction_id_ = 0;
    manifest_ = {};
    setState(makcm::ota::State::Idle, last_activity_ms_);
    if (!persistCommandFact(
            request,
            makcm::ota::snapshot::EffectPhase::ExternalEffectObserved,
            makcm::ota::snapshot::Phase::RolledBack,
            makcm::ota::NackReason::None,
            makcm::ota::State::Idle)) {
        reason = makcm::ota::NackReason::FlashWriteFailed;
        return false;
    }
    if (request_restart) restart_requested_.store(true, std::memory_order_release);
    return true;
}

bool OtaUpdateManager::resetDownload() {
    bool ok = true;
    if (ota_open_) {
        boot_evidence_.last_ota_error = esp_ota_abort(ota_handle_);
        if (boot_evidence_.last_ota_error != ESP_OK) {
            ok = false;
            ESP_LOGE(kTag, "esp_ota_abort failed: %s (%d)",
                     esp_err_to_name(boot_evidence_.last_ota_error),
                     static_cast<int>(boot_evidence_.last_ota_error));
        }
    }
    ota_open_ = false;
    ota_handle_ = 0;
    update_partition_ = nullptr;
    manifest_valid_ = false;
    if (hash_open_) {
        mbedtls_sha256_free(&image_hash_);
        mbedtls_sha256_init(&image_hash_);
    }
    hash_open_ = false;
    next_offset_ = 0;
    last_data_offset_ = 0;
    last_data_length_ = 0;
    last_data_crc_ = 0;
    last_data_sequence_ = 0;
    std::memset(last_data_sha256_, 0, sizeof(last_data_sha256_));
    return ok;
}

void OtaUpdateManager::poll(uint32_t now_ms) {
    const auto value = state_.load();
    if (value == makcm::ota::State::PendingConfirm &&
        !local_boot_validated_) {
        (void)validateLocalBoot(now_ms);
    }
    if ((value == makcm::ota::State::Maintenance ||
         value == makcm::ota::State::Receiving) &&
        now_ms - last_activity_ms_ > config_.maintenance_timeout_ms) {
        // Silence is not an Abort command. Preserve any active identity and
        // partially written slot, fail-close, and require exact host
        // reconciliation instead of erasing transaction evidence.
        setState(makcm::ota::State::Fault, now_ms);
        (void)saveRecord();
    } else if ((value == makcm::ota::State::PendingConfirm ||
                value == makcm::ota::State::Fault) &&
               !local_boot_validated_ &&
               now_ms - pending_confirm_started_ms_ > config_.confirm_timeout_ms) {
        // Host/pair silence is not permission to invalidate a locally bootable
        // image. Preserve evidence and wait for an explicit recovery action.
        setState(makcm::ota::State::Fault, now_ms);
        (void)saveRecord();
    }
}

uint32_t OtaUpdateManager::recordCrc(const LocalRecord &record) {
    return makcm::ota::crc32(reinterpret_cast<const uint8_t *>(&record),
                             offsetof(LocalRecord, crc));
}

bool OtaUpdateManager::readSnapshotCopy(
    const char *key, uint8_t *wire,
    makcm::ota::snapshot::Snapshot &decoded, bool &present, bool &valid) {
    present = false;
    valid = false;
    if (nvs_ == 0 || key == nullptr || wire == nullptr) return false;
    size_t length = 0;
    boot_evidence_.last_nvs_error = nvs_get_blob(nvs_, key, nullptr, &length);
    if (boot_evidence_.last_nvs_error == ESP_ERR_NVS_NOT_FOUND) return true;
    if (boot_evidence_.last_nvs_error != ESP_OK) return false;
    present = true;
    if (length != makcm::ota::snapshot::kSnapshotWireSize) return true;
    boot_evidence_.last_nvs_error = nvs_get_blob(nvs_, key, wire, &length);
    if (boot_evidence_.last_nvs_error != ESP_OK) return false;
    valid = makcm::ota::snapshot::decodeSnapshot(wire, length, decoded) &&
            decoded.role == config_.role;
    return true;
}

makcm::ota::snapshot::Snapshot &OtaUpdateManager::beginSnapshotMutation() {
    pending_snapshot_ = snapshot_;
    return pending_snapshot_;
}

bool OtaUpdateManager::commitPendingSnapshotV2() {
    using namespace makcm::ota::snapshot;
    // A failed loadSnapshotV2() leaves snapshot_ (and therefore every
    // beginSnapshotMutation() copy) with role 0, so RoleMismatch here is not a
    // transient write error -- it means the board can never persist another
    // fact and every command will NACK FlashWriteFailed until NVS is repaired.
    if (nvs_ == 0) {
        snapshot_commit_fault_ = SnapshotCommitFault::NoNvsHandle;
        return false;
    }
    if (pending_snapshot_.role != config_.role) {
        snapshot_commit_fault_ = SnapshotCommitFault::RoleMismatch;
        return false;
    }
    if (snapshot_.commit_sequence == UINT32_MAX) {
        snapshot_commit_fault_ = SnapshotCommitFault::SequenceSaturated;
        return false;
    }
    pending_snapshot_.commit_sequence = snapshot_.commit_sequence + 1;
    if (!encodeSnapshot(pending_snapshot_, snapshot_write_wire_,
                        sizeof(snapshot_write_wire_))) {
        snapshot_commit_fault_ = SnapshotCommitFault::EncodeRejected;
        return false;
    }
    const int8_t next_copy = selected_snapshot_copy_ == 0 ? 1 : 0;
    const char *key = next_copy == 0 ? kNvsSnapshotA : kNvsSnapshotB;
    boot_evidence_.last_nvs_error = nvs_set_blob(
        nvs_, key, snapshot_write_wire_, sizeof(snapshot_write_wire_));
    if (boot_evidence_.last_nvs_error != ESP_OK) {
        snapshot_commit_fault_ = SnapshotCommitFault::SetBlobFailed;
        return false;
    }
    boot_evidence_.last_nvs_error = nvs_commit(nvs_);
    if (boot_evidence_.last_nvs_error != ESP_OK) {
        snapshot_commit_fault_ = SnapshotCommitFault::CommitFailed;
        return false;
    }

    size_t length = sizeof(snapshot_readback_wire_);
    boot_evidence_.last_nvs_error = nvs_get_blob(
        nvs_, key, snapshot_readback_wire_, &length);
    makcm::ota::snapshot::Snapshot &verified = next_copy == 0
        ? snapshot_candidate_a_
        : snapshot_candidate_b_;
    if (boot_evidence_.last_nvs_error != ESP_OK ||
        length != sizeof(snapshot_readback_wire_) ||
        std::memcmp(snapshot_write_wire_, snapshot_readback_wire_,
                    sizeof(snapshot_write_wire_)) != 0 ||
        !decodeSnapshot(snapshot_readback_wire_, length, verified) ||
        verified.commit_sequence != pending_snapshot_.commit_sequence) {
        snapshot_commit_fault_ = SnapshotCommitFault::ReadbackMismatch;
        return false;
    }
    snapshot_commit_fault_ = SnapshotCommitFault::None;
    snapshot_ = pending_snapshot_;
    selected_snapshot_copy_ = next_copy;
    snapshot_ready_ = true;
    snapshot_crc32_ = makcm::ota::readLe32(
        snapshot_readback_wire_ + makcm::ota::snapshot::kCrcOffset);
    return true;
}

bool OtaUpdateManager::loadSnapshotV2() {
    using namespace makcm::ota::snapshot;
    bool present_a = false;
    bool present_b = false;
    bool valid_a = false;
    bool valid_b = false;
    if (!readSnapshotCopy(kNvsSnapshotA, snapshot_wire_a_,
                          snapshot_candidate_a_, present_a, valid_a) ||
        !readSnapshotCopy(kNvsSnapshotB, snapshot_wire_b_,
                          snapshot_candidate_b_, present_b, valid_b))
        return false;
    snapshot_copy_error_ = (present_a && !valid_a) ||
                           (present_b && !valid_b);
    const CopySelection selection = selectSnapshotCopy(
        present_a, valid_a, snapshot_candidate_a_, snapshot_wire_a_,
        present_b, valid_b, snapshot_candidate_b_, snapshot_wire_b_);
    if (selection == CopySelection::Fault) return false;
    if (selection == CopySelection::Initialize) {
        std::memset(&snapshot_, 0, sizeof(snapshot_));
        snapshot_.role = config_.role;
        selected_snapshot_copy_ = -1;
        pending_snapshot_ = snapshot_;
        return commitPendingSnapshotV2();
    }
    if (selection == CopySelection::A) {
        snapshot_ = snapshot_candidate_a_;
        selected_snapshot_copy_ = 0;
    } else {
        snapshot_ = snapshot_candidate_b_;
        selected_snapshot_copy_ = 1;
    }
    snapshot_ready_ = true;
    const uint8_t *selected_wire = selected_snapshot_copy_ == 0
        ? snapshot_wire_a_ : snapshot_wire_b_;
    snapshot_crc32_ = makcm::ota::readLe32(
        selected_wire + makcm::ota::snapshot::kCrcOffset);
    return true;
}

const esp_partition_t *OtaUpdateManager::partitionForSubtype(
    uint8_t subtype) const {
    if (subtype == 0xFF) return nullptr;
    return esp_partition_find_first(
        ESP_PARTITION_TYPE_APP,
        static_cast<esp_partition_subtype_t>(subtype), nullptr);
}

bool OtaUpdateManager::identityDigestMatches(
    const makcm::ota::snapshot::Identity &identity) const {
    uint8_t canonical[180]{};
    makcm::ota::writeLe16(canonical, identity.protocol_epoch);
    canonical[2] = static_cast<uint8_t>(identity.role);
    canonical[3] = identity.target_slot;
    canonical[4] = identity.previous_slot;
    makcm::ota::writeLe32(canonical + 8, identity.transaction_id);
    makcm::ota::writeLe32(canonical + 12, identity.generation);
    makcm::ota::writeLe32(canonical + 16, identity.previous_generation);
    makcm::ota::writeLe32(canonical + 20, identity.partition_schema);
    makcm::ota::writeLe32(canonical + 24, identity.image_size);
    makcm::ota::writeLe32(canonical + 28, identity.min_peer_generation);
    makcm::ota::writeLe32(canonical + 32, identity.max_peer_generation);
    std::memcpy(canonical + 36, identity.hardware_revision, 16);
    std::memcpy(canonical + 52, identity.project_name, 32);
    std::memcpy(canonical + 84, identity.image_sha256, 32);
    std::memcpy(canonical + 116, identity.manifest_sha256, 32);
    std::memcpy(canonical + 148, identity.previous_image_sha256, 32);
    uint8_t digest[32]{};
    return mbedtls_sha256(canonical, sizeof(canonical), digest, 0) == 0 &&
           std::memcmp(digest, identity.identity_digest, sizeof(digest)) == 0;
}

bool OtaUpdateManager::partitionMatchesImage(
    const esp_partition_t *partition,
    const makcm::ota::snapshot::Identity &identity) const {
    if (partition == nullptr ||
        static_cast<uint8_t>(partition->subtype) != identity.target_slot ||
        identity.image_size == 0 || identity.image_size > partition->size)
        return false;
    // identity.image_sha256 covers the full transferred image including the
    // esptool-appended digest trailer.  esp_partition_get_sha256() returns the
    // app-image digest, which excludes that trailer, so the two never compare
    // equal for a hash-appended image; hash the raw partition bytes instead.
    uint8_t digest[32]{};
    mbedtls_sha256_context hash;
    mbedtls_sha256_init(&hash);
    bool ok = mbedtls_sha256_starts(&hash, 0) == 0;
    uint8_t buffer[1024];
    for (uint32_t offset = 0; ok && offset < identity.image_size;) {
        const uint32_t chunk =
            identity.image_size - offset < sizeof(buffer)
                ? identity.image_size - offset
                : static_cast<uint32_t>(sizeof(buffer));
        ok = esp_partition_read(partition, offset, buffer, chunk) == ESP_OK &&
             mbedtls_sha256_update(&hash, buffer, chunk) == 0;
        offset += chunk;
    }
    ok = ok && mbedtls_sha256_finish(&hash, digest) == 0;
    mbedtls_sha256_free(&hash);
    return ok && std::memcmp(digest, identity.image_sha256, sizeof(digest)) == 0;
}

bool OtaUpdateManager::partitionMatchesPreviousImage(
    const esp_partition_t *partition,
    const makcm::ota::snapshot::Identity &identity) const {
    if (partition == nullptr ||
        static_cast<uint8_t>(partition->subtype) != identity.previous_slot)
        return false;
    uint8_t digest[32]{};
    return esp_partition_get_sha256(partition, digest) == ESP_OK &&
           std::memcmp(digest, identity.previous_image_sha256,
                       sizeof(digest)) == 0;
}

bool OtaUpdateManager::restoreFromSnapshotV2(
    uint32_t now_ms, const esp_partition_t *running,
    const esp_partition_t *boot, esp_ota_img_states_t image_state) {
    using namespace makcm::ota;
    using namespace makcm::ota::snapshot;
    local_boot_validated_ = false;
    transaction_id_ = 0;
    next_offset_ = 0;
    manifest_ = {};
    manifest_valid_ = false;
    update_partition_ = nullptr;

    if (!snapshot_.active.present) {
        const auto &completed = snapshot_.completed;
        if (!completed.present) {
            state_.store(State::Idle);
            return true;
        }
        if (!identityDigestMatches(completed.identity))
            return false;
        if (completed.outcome == Outcome::Confirmed &&
            completed.identity.generation == config_.current_generation) {
            if (running == nullptr || boot == nullptr || running != boot ||
                static_cast<uint8_t>(running->subtype) !=
                    completed.identity.target_slot ||
                image_state != ESP_OTA_IMG_VALID ||
                !completed.local_validated || !completed.pair_committed ||
                !partitionMatchesImage(running, completed.identity))
                return false;
            local_boot_validated_ = true;
        } else if (completed.outcome == Outcome::Confirmed &&
                   completed.identity.generation >
                       config_.current_generation) {
            return false;
        } else if (completed.outcome != Outcome::Confirmed) {
            if (completed.identity.previous_generation !=
                    config_.current_generation ||
                running == nullptr || boot == nullptr || running != boot ||
                static_cast<uint8_t>(running->subtype) !=
                    completed.identity.previous_slot ||
                image_state != ESP_OTA_IMG_VALID ||
                !partitionMatchesPreviousImage(running,
                                                completed.identity))
                return false;
            esp_app_desc_t descriptor{};
            if (esp_ota_get_partition_description(running, &descriptor) !=
                    ESP_OK ||
                !exactField(config_.project_name, descriptor.project_name,
                            sizeof(descriptor.project_name)) ||
                !exactGenerationVersion(
                    completed.identity.previous_generation,
                    descriptor.version, sizeof(descriptor.version)))
                return false;
            local_boot_validated_ = true;
        }
        state_.store(State::Idle);
        return true;
    }

    auto &active = snapshot_.active;
    const auto &identity = active.identity;
    if (!identityDigestMatches(identity) || running == nullptr ||
        boot == nullptr || identity.generation < config_.current_generation ||
        identity.partition_schema != config_.partition_schema ||
        !exactField(config_.hardware_revision, identity.hardware_revision,
                    sizeof(identity.hardware_revision)) ||
        !exactField(config_.project_name, identity.project_name,
                    sizeof(identity.project_name)))
        return false;
    const esp_partition_t *target = partitionForSubtype(identity.target_slot);
    const esp_partition_t *previous =
        partitionForSubtype(identity.previous_slot);
    if (target == nullptr || previous == nullptr || target == previous)
        return false;
    const uint8_t running_slot = static_cast<uint8_t>(running->subtype);
    const uint8_t boot_slot = static_cast<uint8_t>(boot->subtype);
    const bool running_previous = running_slot == identity.previous_slot;
    const bool running_target = running_slot == identity.target_slot;
    const bool boot_previous = boot_slot == identity.previous_slot;
    const bool boot_target = boot_slot == identity.target_slot;

    transaction_id_ = identity.transaction_id;
    manifest_.generation = identity.generation;
    manifest_.image_size = identity.image_size;
    std::memcpy(manifest_.image_sha256, identity.image_sha256,
                sizeof(manifest_.image_sha256));
    next_offset_ = active.next_offset;
    update_partition_ = target;
    pending_confirm_started_ms_ = now_ms;

    switch (snapshot_.active.phase) {
    case Phase::Receiving:
        if (!running_previous || !boot_previous ||
            identity.generation <= config_.current_generation)
            return false;
        // A reboot destroys the IDF OTA handle and streaming SHA context.
        // Restarting the exact signed MANIFEST is safe because no DATA is
        // acknowledged until MANIFEST has a durable response.
        state_.store(State::Maintenance);
        next_offset_ = 0;
        return true;
    case Phase::Prepared:
        if (!running_previous || !boot_previous ||
            identity.generation <= config_.current_generation ||
            !partitionMatchesImage(target, identity))
            return false;
        state_.store(State::Prepared);
        next_offset_ = identity.image_size;
        return true;
    case Phase::ActivateIntent:
        if (running_previous && identity.generation >
                                    config_.current_generation &&
            partitionMatchesImage(target, identity)) {
            if (boot_previous) {
                state_.store(State::Prepared);
                return true;
            }
            if (boot_target) {
                auto &next = beginSnapshotMutation();
                auto &entry = next.active.ledger[ledgerIndex(
                    Command::Activate)];
                if (!entry.valid) return false;
                entry.effect_phase = EffectPhase::ExternalEffectObserved;
                entry.result = NackReason::None;
                entry.resulting_state = State::ActivationPending;
                next.active.phase = Phase::BootSelected;
                next.active.running_slot = running_slot;
                next.active.boot_slot = boot_slot;
                if (!commitPendingSnapshotV2()) return false;
                state_.store(State::ActivationPending);
                restart_requested_.store(true, std::memory_order_release);
                return true;
            }
            return false;
        }
        [[fallthrough]];
    case Phase::BootSelected:
        if (running_previous && boot_target &&
            identity.generation > config_.current_generation &&
            partitionMatchesImage(target, identity)) {
            state_.store(State::ActivationPending);
            restart_requested_.store(true, std::memory_order_release);
            return true;
        }
        [[fallthrough]];
    case Phase::PendingVerify:
    case Phase::LocalValidated:
    case Phase::PairPending:
        if (!running_target || !boot_target ||
            identity.generation != config_.current_generation ||
            !partitionMatchesImage(running, identity))
            return false;
        update_partition_ = running;
        if (image_state == ESP_OTA_IMG_PENDING_VERIFY) {
            if (snapshot_.active.local_validated) return false;
            state_.store(State::PendingConfirm);
            return true;
        }
        if (image_state != ESP_OTA_IMG_VALID) return false;
        // VALID is durable bootloader evidence.  It closes the power window
        // after mark-valid and before the LocalValidated snapshot commit.
        local_boot_validated_ = true;
        state_.store(State::PendingConfirm);
        if (!snapshot_.active.local_validated &&
            !persistLocalValidationFact())
            return false;
        return true;
    case Phase::RollbackIntent:
    case Phase::RolledBack:
        if (!running_previous || !boot_previous ||
            image_state != ESP_OTA_IMG_VALID ||
            !partitionMatchesPreviousImage(running, identity))
            return false;
        {
            esp_app_desc_t descriptor{};
            if (esp_ota_get_partition_description(running, &descriptor) !=
                    ESP_OK ||
                !exactField(config_.project_name, descriptor.project_name,
                            sizeof(descriptor.project_name)) ||
                !exactGenerationVersion(identity.previous_generation,
                                        descriptor.version,
                                        sizeof(descriptor.version)))
                return false;
        }
        if (snapshot_.active.phase == Phase::RollbackIntent) {
            auto &next = beginSnapshotMutation();
            auto &entry =
                next.active.ledger[ledgerIndex(Command::Abort)];
            if (!entry.valid) return false;
            entry.effect_phase = EffectPhase::ExternalEffectObserved;
            entry.result = NackReason::None;
            entry.resulting_state = State::Idle;
            next.active.phase = Phase::RolledBack;
            next.active.running_slot = running_slot;
            next.active.boot_slot = boot_slot;
            next.active.image_state = static_cast<uint8_t>(image_state);
            if (!commitPendingSnapshotV2()) return false;
        }
        state_.store(State::Idle);
        return true;
    case Phase::Failed:
        state_.store(State::Fault);
        return true;
    case Phase::Empty:
    case Phase::PairCommitted:
        return false;
    }
    return false;
}

bool OtaUpdateManager::persistLocalValidationFact() {
    using namespace makcm::ota::snapshot;
    if (!snapshot_.active.present || transaction_id_ == 0 ||
        snapshot_.active.identity.transaction_id != transaction_id_ ||
        snapshot_.active.identity.generation != config_.current_generation ||
        update_partition_ == nullptr ||
        static_cast<uint8_t>(update_partition_->subtype) !=
            snapshot_.active.identity.target_slot ||
        !partitionMatchesImage(update_partition_, snapshot_.active.identity))
        return false;
    auto &next = beginSnapshotMutation();
    next.active.phase = Phase::LocalValidated;
    next.active.local_validated = true;
    next.active.running_slot = boot_evidence_.running_subtype;
    next.active.boot_slot = boot_evidence_.boot_subtype;
    next.active.image_state = static_cast<uint8_t>(ESP_OTA_IMG_VALID);
    next.active.reset_reason =
        static_cast<uint32_t>(boot_evidence_.reset_reason);
    next.active.last_ota_error =
        static_cast<int32_t>(boot_evidence_.mark_valid_error);
    next.active.last_state_error =
        static_cast<int32_t>(boot_evidence_.post_state_error);
    return commitPendingSnapshotV2();
}

bool OtaUpdateManager::hashRequest(const makcm::ota::Frame &request,
                                   uint8_t out[32]) const {
    if (out == nullptr || request.length > makcm::ota::kMaxChunkSize)
        return false;
    uint8_t header[20]{};
    header[0] = static_cast<uint8_t>(request.target);
    header[1] = static_cast<uint8_t>(request.command);
    header[2] = static_cast<uint8_t>(request.flags &
                                     ~makcm::ota::kFlagRetransmit);
    makcm::ota::writeLe32(header + 4, request.transaction_id);
    makcm::ota::writeLe32(header + 8, request.offset);
    makcm::ota::writeLe32(header + 12, request.sequence);
    makcm::ota::writeLe16(header + 16, request.length);
    mbedtls_sha256_context hash;
    mbedtls_sha256_init(&hash);
    bool ok = mbedtls_sha256_starts(&hash, 0) == 0 &&
              mbedtls_sha256_update(&hash, header, sizeof(header)) == 0 &&
              (request.length == 0 ||
               mbedtls_sha256_update(&hash, request.payload,
                                     request.length) == 0) &&
              mbedtls_sha256_finish(&hash, out) == 0;
    mbedtls_sha256_free(&hash);
    return ok;
}

bool OtaUpdateManager::buildActiveIdentity(
    const makcm::ota::Frame &manifest_request) {
    using namespace makcm::ota::snapshot;
    if (update_partition_ == nullptr || !manifest_valid_ ||
        manifest_request.command != makcm::ota::Command::Manifest ||
        manifest_request.transaction_id == 0) return false;
    auto &active = pending_snapshot_.active;
    std::memset(&active, 0, sizeof(active));
    active.present = true;
    active.phase = Phase::Receiving;
    active.running_slot = boot_evidence_.running_subtype;
    active.boot_slot = boot_evidence_.boot_subtype;
    active.image_state = static_cast<uint8_t>(boot_evidence_.pre_state);
    active.reset_reason = static_cast<uint32_t>(boot_evidence_.reset_reason);
    active.identity.protocol_epoch = kProtocolEpoch;
    active.identity.role = config_.role;
    active.identity.target_slot =
        static_cast<uint8_t>(update_partition_->subtype);
    const esp_partition_t *running = esp_ota_get_running_partition();
    active.identity.previous_slot = running == nullptr
        ? 0xFF
        : static_cast<uint8_t>(running->subtype);
    if (running == nullptr ||
        esp_partition_get_sha256(
            running, active.identity.previous_image_sha256) != ESP_OK)
        return false;
    active.identity.transaction_id = manifest_request.transaction_id;
    active.identity.generation = manifest_.generation;
    active.identity.previous_generation = config_.current_generation;
    active.identity.partition_schema = manifest_.partition_schema;
    active.identity.image_size = manifest_.image_size;
    active.identity.min_peer_generation = manifest_.min_peer_generation;
    active.identity.max_peer_generation = manifest_.max_peer_generation;
    std::memcpy(active.identity.hardware_revision,
                manifest_.hardware_revision,
                sizeof(active.identity.hardware_revision));
    std::memcpy(active.identity.project_name, manifest_.project_name,
                sizeof(active.identity.project_name));
    std::memcpy(active.identity.image_sha256, manifest_.image_sha256,
                sizeof(active.identity.image_sha256));
    if (mbedtls_sha256(manifest_request.payload, manifest_request.length,
                       active.identity.manifest_sha256, 0) != 0)
        return false;
    uint8_t canonical[180]{};
    makcm::ota::writeLe16(canonical, active.identity.protocol_epoch);
    canonical[2] = static_cast<uint8_t>(active.identity.role);
    canonical[3] = active.identity.target_slot;
    canonical[4] = active.identity.previous_slot;
    makcm::ota::writeLe32(canonical + 8, active.identity.transaction_id);
    makcm::ota::writeLe32(canonical + 12, active.identity.generation);
    makcm::ota::writeLe32(canonical + 16,
                          active.identity.previous_generation);
    makcm::ota::writeLe32(canonical + 20,
                          active.identity.partition_schema);
    makcm::ota::writeLe32(canonical + 24, active.identity.image_size);
    makcm::ota::writeLe32(canonical + 28,
                          active.identity.min_peer_generation);
    makcm::ota::writeLe32(canonical + 32,
                          active.identity.max_peer_generation);
    std::memcpy(canonical + 36, active.identity.hardware_revision, 16);
    std::memcpy(canonical + 52, active.identity.project_name, 32);
    std::memcpy(canonical + 84, active.identity.image_sha256, 32);
    std::memcpy(canonical + 116, active.identity.manifest_sha256, 32);
    std::memcpy(canonical + 148, active.identity.previous_image_sha256, 32);
    return mbedtls_sha256(canonical, sizeof(canonical),
                          active.identity.identity_digest, 0) == 0;
}

void OtaUpdateManager::fillReplayResponse(
    const makcm::ota::Frame &request,
    const makcm::ota::snapshot::LedgerEntry &entry,
    makcm::ota::Frame &response) const {
    std::memset(&response, 0, sizeof(response));
    response.target = makcm::ota::Target::Host;
    response.command = entry.result == makcm::ota::NackReason::None
        ? makcm::ota::Command::Ack
        : makcm::ota::Command::Nack;
    response.flags = makcm::ota::kFlagResponse;
    response.transaction_id = request.transaction_id;
    response.sequence = request.sequence;
    response.length = entry.response_length;
    if (entry.response_length != 0) {
        std::memcpy(response.payload, entry.response, entry.response_length);
        if (entry.response_length >= makcm::ota::kStatusPayloadSize)
            response.offset = makcm::ota::readLe32(entry.response + 8);
    }
}

void OtaUpdateManager::fillTombstoneResponse(
    const makcm::ota::Frame &request,
    const makcm::ota::snapshot::CompletedTombstone &completed,
    makcm::ota::Frame &response) const {
    std::memset(&response, 0, sizeof(response));
    response.target = makcm::ota::Target::Host;
    response.command = completed.result == makcm::ota::NackReason::None
        ? makcm::ota::Command::Ack
        : makcm::ota::Command::Nack;
    response.flags = makcm::ota::kFlagResponse;
    response.transaction_id = request.transaction_id;
    response.sequence = request.sequence;
    response.length = completed.response_length;
    if (completed.response_length != 0) {
        std::memcpy(response.payload, completed.response,
                    completed.response_length);
        if (completed.response_length >= makcm::ota::kStatusPayloadSize)
            response.offset = makcm::ota::readLe32(completed.response + 8);
    }
}

OtaUpdateManager::ReplayDecision OtaUpdateManager::replayCommand(
    const makcm::ota::Frame &request, makcm::ota::Frame &response) const {
    using namespace makcm::ota::snapshot;
    const size_t index = ledgerIndex(request.command);
    if (!snapshot_ready_ || index >= kLedgerCount) return ReplayDecision::NewCommand;
    uint8_t request_hash[32]{};
    if (!hashRequest(request, request_hash)) return ReplayDecision::Conflict;

    if (snapshot_.completed.present &&
        request.transaction_id ==
            snapshot_.completed.identity.transaction_id) {
        const auto &completed = snapshot_.completed;
        if (request.command == completed.terminal_command &&
            request.sequence == completed.terminal_sequence &&
            std::memcmp(request_hash, completed.terminal_request_sha256,
                        sizeof(request_hash)) == 0) {
            if (completed.response_length == 0)
                return ReplayDecision::Conflict;
            fillTombstoneResponse(request, completed, response);
            return ReplayDecision::Replayed;
        }
        return ReplayDecision::Conflict;
    }

    if (!snapshot_.active.present ||
        request.transaction_id != snapshot_.active.identity.transaction_id)
        return ReplayDecision::NewCommand;
    const auto &entry = snapshot_.active.ledger[index];
    if (entry.valid) {
        if (entry.command != request.command || entry.sequence != request.sequence ||
            std::memcmp(request_hash, entry.request_sha256,
                        sizeof(request_hash)) != 0)
            return ReplayDecision::Conflict;
        // A completed MANIFEST ACK does not make the IDF OTA handle durable.
        // After reboot, replay the exact signed MANIFEST to recreate the
        // erased-slot handle and streaming hash before acknowledging DATA.
        if (entry.command == makcm::ota::Command::Manifest && !ota_open_ &&
            state_.load() == makcm::ota::State::Maintenance)
            return ReplayDecision::ResumeIntent;
        if (entry.effect_phase == EffectPhase::IntentDurable)
            return ReplayDecision::ResumeIntent;
        if (entry.effect_phase == EffectPhase::ExternalEffectObserved)
            return ReplayDecision::ResumeObserved;
        if (entry.effect_phase != EffectPhase::Done ||
            entry.response_length == 0)
            return ReplayDecision::Conflict;
        fillReplayResponse(request, entry, response);
        return ReplayDecision::Replayed;
    }
    for (const auto &other : snapshot_.active.ledger) {
        if (other.valid && other.sequence == request.sequence)
            return ReplayDecision::Conflict;
    }
    if (request.sequence == 0 ||
        request.sequence <= snapshot_.active.last_request_sequence)
        return ReplayDecision::Conflict;
    return ReplayDecision::NewCommand;
}

bool OtaUpdateManager::resumeIncompleteCommand(
    const makcm::ota::Frame &request, ReplayDecision decision,
    makcm::ota::Frame &response, uint32_t now_ms,
    makcm::ota::NackReason &reason) {
    using namespace makcm::ota;
    using namespace makcm::ota::snapshot;
    const size_t index = ledgerIndex(request.command);
    if (!snapshot_.active.present || index >= kLedgerCount ||
        snapshot_.active.identity.transaction_id != request.transaction_id) {
        reason = NackReason::WrongTransaction;
        fillResponse(request, Command::Nack, reason, response);
        return false;
    }

    auto complete_observed = [&]() -> bool {
        if (request.command == Command::Activate) {
            if (state_.load() == State::Prepared) {
                setState(State::ActivationPending, now_ms);
                restart_requested_.store(true, std::memory_order_release);
            }
        } else if (request.command == Command::Abort) {
            setState(State::Idle, now_ms);
        }
        fillResponse(request, Command::Ack, NackReason::None, response);
        if (!finalizeCommandResponse(request, response, NackReason::None,
                                     true)) {
            reason = NackReason::FlashWriteFailed;
            fillResponse(request, Command::Nack, reason, response);
            return false;
        }
        reason = NackReason::None;
        return true;
    };

    if (decision == ReplayDecision::ResumeObserved) {
        // A MANIFEST fact cannot restore the volatile IDF OTA handle across a
        // reboot. Re-running the exact signed manifest is the safe recovery;
        // same-uptime retries can use the already-open handle and stored fact.
        if (request.command != Command::Manifest || ota_open_)
            return complete_observed();
        setState(State::Maintenance, now_ms);
        decision = ReplayDecision::ResumeIntent;
    }

    bool ok = false;
    switch (request.command) {
    case Command::Manifest:
        if (state_.load() != State::Maintenance) {
            reason = NackReason::InvalidState;
            break;
        }
        ok = acceptManifest(request, now_ms, reason);
        break;
    case Command::Prepare:
        if (state_.load() == State::Prepared && update_partition_ != nullptr &&
            partitionMatchesImage(update_partition_,
                                  snapshot_.active.identity)) {
            if (!persistCommandFact(
                    request, EffectPhase::ExternalEffectObserved,
                    Phase::Prepared, NackReason::None, State::Prepared)) {
                reason = NackReason::FlashWriteFailed;
                break;
            }
            return complete_observed();
        }
        ok = prepare(request, now_ms, reason);
        break;
    case Command::Activate:
        if (state_.load() != State::Prepared) {
            reason = NackReason::InvalidState;
            break;
        }
        ok = activate(request, now_ms, reason);
        break;
    case Command::Confirm:
        ok = confirm(request, now_ms, reason);
        break;
    case Command::Abort:
        ok = abort(request, true, reason);
        break;
    default:
        reason = NackReason::InvalidState;
        break;
    }

    fillResponse(request, ok ? Command::Ack : Command::Nack, reason, response);
    if (!finalizeCommandResponse(request, response, reason, ok)) {
        reason = NackReason::FlashWriteFailed;
        fillResponse(request, Command::Nack, reason, response);
        return false;
    }
    return ok;
}

bool OtaUpdateManager::setLedgerIntent(
    makcm::ota::snapshot::ActiveTransaction &active,
    const makcm::ota::Frame &request, makcm::ota::State accepted_from,
    makcm::ota::snapshot::Phase phase) {
    using namespace makcm::ota::snapshot;
    const size_t index = ledgerIndex(request.command);
    if (!active.present || index >= kLedgerCount || request.sequence == 0)
        return false;
    auto &entry = active.ledger[index];
    std::memset(&entry, 0, sizeof(entry));
    entry.valid = true;
    entry.command = request.command;
    entry.effect_phase = EffectPhase::IntentDurable;
    entry.accepted_from_state = accepted_from;
    entry.sequence = request.sequence;
    if (!hashRequest(request, entry.request_sha256)) return false;
    entry.resulting_state = accepted_from;
    active.phase = phase;
    active.last_request_sequence = request.sequence;
    active.last_command = request.command;
    active.last_nack = makcm::ota::NackReason::None;
    active.reset_reason = static_cast<uint32_t>(boot_evidence_.reset_reason);
    active.last_ota_error = static_cast<int32_t>(boot_evidence_.last_ota_error);
    active.last_state_error =
        static_cast<int32_t>(boot_evidence_.post_state_error);
    return true;
}

bool OtaUpdateManager::persistCommandIntent(
    const makcm::ota::Frame &request, makcm::ota::State accepted_from,
    makcm::ota::snapshot::Phase phase) {
    auto &next = beginSnapshotMutation();
    if (!next.active.present ||
        next.active.identity.transaction_id != request.transaction_id ||
        !setLedgerIntent(next.active, request, accepted_from, phase))
        return false;
    return commitPendingSnapshotV2();
}

bool OtaUpdateManager::persistCommandFact(
    const makcm::ota::Frame &request,
    makcm::ota::snapshot::EffectPhase effect,
    makcm::ota::snapshot::Phase phase, makcm::ota::NackReason result,
    makcm::ota::State resulting_state) {
    using namespace makcm::ota::snapshot;
    const size_t index = ledgerIndex(request.command);
    if (index >= kLedgerCount) return false;
    uint8_t request_hash[32]{};
    if (!hashRequest(request, request_hash)) return false;
    auto &next = beginSnapshotMutation();
    if (!next.active.present ||
        next.active.identity.transaction_id != request.transaction_id)
        return false;
    auto &entry = next.active.ledger[index];
    if (!entry.valid || entry.command != request.command ||
        entry.sequence != request.sequence ||
        std::memcmp(entry.request_sha256, request_hash,
                    sizeof(request_hash)) != 0)
        return false;
    entry.effect_phase = effect;
    entry.result = result;
    entry.resulting_state = resulting_state;
    next.active.phase = phase;
    next.active.next_offset = next_offset_;
    next.active.running_slot = boot_evidence_.running_subtype;
    next.active.boot_slot = boot_evidence_.boot_subtype;
    next.active.image_state = static_cast<uint8_t>(boot_evidence_.post_state);
    // active.local_validated is a property of THIS transaction -- "the image
    // this transaction installed has booted and validated" -- not of the board.
    // validSnapshot() enforces exactly that by requiring the flag to equal
    // `phase is LocalValidated or PairPending`, so writing the board-wide
    // local_boot_validated_ here made every fact persisted in any other phase
    // fail its own encoder.  On a healthy board that flag is true, so the first
    // durable fact after MANIFEST was rejected before any NVS call and surfaced
    // as the opaque FlashWriteFailed that blocked every V2 pair OTA.
    // validateLocalBoot() remains the one place that sets the flag, together
    // with the phase that justifies it.
    next.active.local_validated =
        phase == makcm::ota::snapshot::Phase::LocalValidated ||
        phase == makcm::ota::snapshot::Phase::PairPending;
    next.active.last_nack = result;
    next.active.last_ota_error =
        static_cast<int32_t>(boot_evidence_.last_ota_error);
    next.active.last_state_error =
        static_cast<int32_t>(boot_evidence_.post_state_error);
    return commitPendingSnapshotV2();
}

bool OtaUpdateManager::finalizeCommandResponse(
    const makcm::ota::Frame &request, const makcm::ota::Frame &response,
    makcm::ota::NackReason result, bool accepted) {
    using namespace makcm::ota::snapshot;
    const size_t index = ledgerIndex(request.command);
    if (!snapshot_ready_ || index >= kLedgerCount) return true;
    if (!snapshot_.active.present ||
        snapshot_.active.identity.transaction_id != request.transaction_id)
        return true;
    uint8_t request_hash[32]{};
    if (!hashRequest(request, request_hash)) return false;
    auto &next = beginSnapshotMutation();
    auto &entry = next.active.ledger[index];
    if (!entry.valid || entry.sequence != request.sequence ||
        entry.command != request.command ||
        std::memcmp(entry.request_sha256, request_hash,
                    sizeof(request_hash)) != 0)
        return true;
    entry.effect_phase = EffectPhase::Done;
    entry.result = result;
    entry.resulting_state = state_.load();
    entry.response_length = response.length <= makcm::ota::kStatusPayloadSize
        ? static_cast<uint8_t>(response.length)
        : 0;
    entry.snapshot_commit_sequence = snapshot_.commit_sequence + 1;
    if (entry.response_length != 0)
        std::memcpy(entry.response, response.payload, entry.response_length);

    if (accepted && (request.command == makcm::ota::Command::Confirm ||
                     request.command == makcm::ota::Command::Abort)) {
        auto &completed = next.completed;
        std::memset(&completed, 0, sizeof(completed));
        completed.present = true;
        completed.outcome = request.command == makcm::ota::Command::Confirm
            ? Outcome::Confirmed
            : Outcome::Aborted;
        completed.terminal_command = request.command;
        completed.final_state = request.command == makcm::ota::Command::Confirm
            ? makcm::ota::State::Confirmed
            : makcm::ota::State::Idle;
        completed.terminal_sequence = request.sequence;
        completed.result = result;
        completed.response_length = entry.response_length;
        const esp_partition_t *running = esp_ota_get_running_partition();
        const esp_partition_t *boot = esp_ota_get_boot_partition();
        completed.running_slot = running == nullptr
            ? 0xFF : static_cast<uint8_t>(running->subtype);
        completed.boot_slot = boot == nullptr
            ? 0xFF : static_cast<uint8_t>(boot->subtype);
        completed.image_state = static_cast<uint8_t>(boot_evidence_.post_state);
        completed.local_validated = local_boot_validated_;
        completed.pair_committed = request.command ==
            makcm::ota::Command::Confirm;
        completed.identity = next.active.identity;
        std::memcpy(completed.terminal_request_sha256, request_hash,
                    sizeof(request_hash));
        if (entry.response_length != 0)
            std::memcpy(completed.response, entry.response,
                        entry.response_length);
        std::memset(&next.active, 0, sizeof(next.active));
    }
    return commitPendingSnapshotV2();
}

bool OtaUpdateManager::saveRecord() {
    if (nvs_ == 0 || transaction_id_ == 0) return false;
    LocalRecord record{};
    record.magic = kRecordMagic;
    record.version = kRecordVersion;
    record.state = static_cast<uint8_t>(state_.load());
    record.partition_subtype = update_partition_ == nullptr
        ? 0
        : static_cast<uint8_t>(update_partition_->subtype);
    record.transaction_id = transaction_id_;
    record.target_generation = manifest_.generation;
    return writeRecord(kNvsRecord, record);
}

bool OtaUpdateManager::saveCompletedRecord() {
    if (nvs_ == 0 || transaction_id_ == 0 || manifest_.generation == 0)
        return false;
    LocalRecord record{};
    record.magic = kRecordMagic;
    record.version = kRecordVersion;
    record.state = static_cast<uint8_t>(makcm::ota::State::Confirmed);
    const esp_partition_t *running = esp_ota_get_running_partition();
    record.partition_subtype = running == nullptr
        ? 0
        : static_cast<uint8_t>(running->subtype);
    record.transaction_id = transaction_id_;
    record.target_generation = manifest_.generation;
    if (!writeRecord(kNvsCompletedRecord, record)) return false;
    completed_record_ = record;
    completed_record_valid_ = true;
    return true;
}

bool OtaUpdateManager::writeRecord(const char *key,
                                   const LocalRecord &source) {
    if (nvs_ == 0 || key == nullptr) return false;
    LocalRecord record = source;
    record.crc = recordCrc(record);
    boot_evidence_.last_nvs_error =
        nvs_set_blob(nvs_, key, &record, sizeof(record));
    if (boot_evidence_.last_nvs_error != ESP_OK) return false;
    boot_evidence_.last_nvs_error = nvs_commit(nvs_);
    return boot_evidence_.last_nvs_error == ESP_OK;
}

OtaUpdateManager::RecordLoad OtaUpdateManager::loadRecord(
    const char *key, LocalRecord &record) {
    if (nvs_ == 0 || key == nullptr) return RecordLoad::IoError;
    size_t length = sizeof(record);
    boot_evidence_.last_nvs_error =
        nvs_get_blob(nvs_, key, &record, &length);
    if (boot_evidence_.last_nvs_error == ESP_ERR_NVS_NOT_FOUND)
        return RecordLoad::Missing;
    if (boot_evidence_.last_nvs_error != ESP_OK)
        return boot_evidence_.last_nvs_error == ESP_ERR_NVS_INVALID_LENGTH
            ? RecordLoad::Invalid
            : RecordLoad::IoError;
    if (length != sizeof(record) || record.magic != kRecordMagic ||
        record.version != kRecordVersion || record.transaction_id == 0 ||
        record.target_generation == 0 || record.crc != recordCrc(record) ||
        record.state > static_cast<uint8_t>(makcm::ota::State::Fault))
        return RecordLoad::Invalid;
    return RecordLoad::Valid;
}

bool OtaUpdateManager::clearRecord() {
    if (nvs_ == 0) return false;
    boot_evidence_.last_nvs_error = nvs_erase_key(nvs_, kNvsRecord);
    if (boot_evidence_.last_nvs_error != ESP_OK &&
        boot_evidence_.last_nvs_error != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(kTag, "nvs_erase_key failed: %s (%d)",
                 esp_err_to_name(boot_evidence_.last_nvs_error),
                 static_cast<int>(boot_evidence_.last_nvs_error));
        return false;
    }
    boot_evidence_.last_nvs_error = nvs_commit(nvs_);
    if (boot_evidence_.last_nvs_error != ESP_OK) {
        ESP_LOGE(kTag, "nvs_commit(clear) failed: %s (%d)",
                 esp_err_to_name(boot_evidence_.last_nvs_error),
                 static_cast<int>(boot_evidence_.last_nvs_error));
    }
    return boot_evidence_.last_nvs_error == ESP_OK;
}

} // namespace makcm::native
