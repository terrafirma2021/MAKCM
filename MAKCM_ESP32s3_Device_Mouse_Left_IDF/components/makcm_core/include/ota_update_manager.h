#pragma once

#include <atomic>
#include <stdint.h>

#include "OtaManifest.h"
#include "OtaImageIdentity.h"
#include "OtaProtocol.h"
#include "OtaSnapshotV2.h"
#include "OtaStatusV2.h"

#include "esp_ota_ops.h"
#include "esp_system.h"
#include "mbedtls/sha256.h"
#include "nvs.h"

namespace makcm::native {

struct OtaTrustAnchor {
    uint8_t key_id[16]{};
    uint8_t public_x[32]{};
    uint8_t public_y[32]{};

    bool configured() const;
};

struct OtaUpdateConfig {
    makcm::ota::Target role = makcm::ota::Target::Left;
    uint16_t protocol_version = 1;
    uint32_t current_generation = 1;
    uint32_t peer_generation = 1;
    uint32_t partition_schema = 0;
    const char *hardware_revision = nullptr;
    const char *project_name = nullptr;
    uint32_t maintenance_timeout_ms = 15000;
    uint32_t confirm_timeout_ms = 30000;
    OtaTrustAnchor trust{};
    bool (*quiesce)(void *context) = nullptr;
    // Local boot health controls whether the bootloader rollback latch may be
    // cancelled. Pair health is deliberately separate: a missing controller
    // or peer link must keep the proxy quiesced, not kill a bootable image.
    bool (*local_health_check)(void *context) = nullptr;
    bool (*pair_health_check)(void *context) = nullptr;
    void *context = nullptr;
};

class OtaUpdateManager final {
public:
    struct BootEvidence {
        esp_reset_reason_t reset_reason = ESP_RST_UNKNOWN;
        esp_err_t pre_state_error = ESP_OK;
        esp_err_t mark_valid_error = ESP_OK;
        esp_err_t post_state_error = ESP_OK;
        esp_err_t last_ota_error = ESP_OK;
        esp_err_t last_nvs_error = ESP_OK;
        esp_ota_img_states_t pre_state = ESP_OTA_IMG_UNDEFINED;
        esp_ota_img_states_t post_state = ESP_OTA_IMG_UNDEFINED;
        uint8_t running_subtype = 0xFF;
        uint8_t boot_subtype = 0xFF;
        bool running_equals_boot = false;
    };

    OtaUpdateManager() = default;
    ~OtaUpdateManager();
    OtaUpdateManager(const OtaUpdateManager &) = delete;
    OtaUpdateManager &operator=(const OtaUpdateManager &) = delete;

    bool begin(const OtaUpdateConfig &config, uint32_t now_ms);
    bool handle(const makcm::ota::Frame &request,
                makcm::ota::Frame &response, uint32_t now_ms);
    void poll(uint32_t now_ms);

    makcm::ota::State state() const { return state_.load(); }
    uint32_t transactionId() const { return transaction_id_; }
    uint32_t nextOffset() const { return next_offset_; }
    bool maintenanceExclusive() const;
    bool takeRestartRequest();
    bool trustConfigured() const { return config_.trust.configured(); }
    bool localBootValidated() const { return local_boot_validated_; }
    const BootEvidence &bootEvidence() const { return boot_evidence_; }

    // Every durable OTA command funnels through commitPendingSnapshotV2(), and
    // every way it can fail is reported to the host as the one opaque
    // NackReason::FlashWriteFailed.  Four of those ways return before any
    // esp_err_t exists, so last_nvs_error stays at whatever a benign earlier
    // read left behind.  Name the site instead: a rejected MANIFEST and a
    // rejected ABORT are the same symptom but very different faults.
    enum class SnapshotCommitFault : uint8_t {
        None = 0,
        NoNvsHandle = 1,
        RoleMismatch = 2,
        SequenceSaturated = 3,
        EncodeRejected = 4,
        SetBlobFailed = 5,
        CommitFailed = 6,
        ReadbackMismatch = 7,
    };
    SnapshotCommitFault snapshotCommitFault() const {
        return snapshot_commit_fault_;
    }

private:
    enum class RecordLoad : uint8_t {
        Missing,
        Valid,
        Invalid,
        IoError,
    };

    struct LocalRecord {
        uint32_t magic = 0;
        uint16_t version = 0;
        uint8_t state = 0;
        uint8_t partition_subtype = 0;
        uint32_t transaction_id = 0;
        uint32_t target_generation = 0;
        uint32_t crc = 0;
    };

    static constexpr uint32_t kRecordMagic = 0x3141544Fu; // "OTA1"
    static constexpr uint16_t kRecordVersion = 1;

    bool enter(const makcm::ota::Frame &request, uint32_t now_ms,
               makcm::ota::NackReason &reason);
    bool acceptManifest(const makcm::ota::Frame &request, uint32_t now_ms,
                        makcm::ota::NackReason &reason);
    bool acceptData(const makcm::ota::Frame &request, uint32_t now_ms,
                    makcm::ota::NackReason &reason);
    bool prepare(const makcm::ota::Frame &request, uint32_t now_ms,
                 makcm::ota::NackReason &reason);
    bool activate(const makcm::ota::Frame &request, uint32_t now_ms,
                  makcm::ota::NackReason &reason);
    bool confirm(const makcm::ota::Frame &request, uint32_t now_ms,
                 makcm::ota::NackReason &reason);
    bool validateLocalBoot(uint32_t now_ms);
    bool abort(const makcm::ota::Frame &request, bool request_restart,
               makcm::ota::NackReason &reason);
    bool resetDownload();
    void setState(makcm::ota::State state, uint32_t now_ms);
    void fillResponse(const makcm::ota::Frame &request,
                      makcm::ota::Command response_command,
                      makcm::ota::NackReason reason,
                      makcm::ota::Frame &response) const;
    bool fillStatusV2(const makcm::ota::Frame &request,
                      makcm::ota::Frame &response);
    uint32_t statusFlags() const;
    bool validateFirstChunk(const uint8_t *data, size_t length) const;
    bool validateEmbeddedIdentity(const esp_partition_t *partition,
                                  uint32_t image_size,
                                  uint32_t generation) const;
    bool verifyManifestSignature(const uint8_t *wire) const;
    bool saveRecord();
    bool saveCompletedRecord();
    bool writeRecord(const char *key, const LocalRecord &record);
    RecordLoad loadRecord(const char *key, LocalRecord &record);
    bool clearRecord();
    static uint32_t recordCrc(const LocalRecord &record);
    bool loadSnapshotV2();
    bool readSnapshotCopy(const char *key, uint8_t *wire,
                          makcm::ota::snapshot::Snapshot &decoded,
                          bool &present, bool &valid);
    bool commitPendingSnapshotV2();
    makcm::ota::snapshot::Snapshot &beginSnapshotMutation();
    bool restoreFromSnapshotV2(uint32_t now_ms,
                               const esp_partition_t *running,
                               const esp_partition_t *boot,
                               esp_ota_img_states_t image_state);
    const esp_partition_t *partitionForSubtype(uint8_t subtype) const;
    bool partitionMatchesImage(
        const esp_partition_t *partition,
        const makcm::ota::snapshot::Identity &identity) const;
    bool partitionMatchesPreviousImage(
        const esp_partition_t *partition,
        const makcm::ota::snapshot::Identity &identity) const;
    bool identityDigestMatches(
        const makcm::ota::snapshot::Identity &identity) const;
    bool persistLocalValidationFact();
    bool buildActiveIdentity(const makcm::ota::Frame &manifest_request);
    bool hashRequest(const makcm::ota::Frame &request, uint8_t out[32]) const;
    enum class ReplayDecision : uint8_t {
        NewCommand,
        Replayed,
        Conflict,
        ResumeIntent,
        ResumeObserved,
    };
    ReplayDecision replayCommand(const makcm::ota::Frame &request,
                                 makcm::ota::Frame &response) const;
    bool resumeIncompleteCommand(const makcm::ota::Frame &request,
                                 ReplayDecision decision,
                                 makcm::ota::Frame &response,
                                 uint32_t now_ms,
                                 makcm::ota::NackReason &reason);
    bool setLedgerIntent(makcm::ota::snapshot::ActiveTransaction &active,
                         const makcm::ota::Frame &request,
                         makcm::ota::State accepted_from,
                         makcm::ota::snapshot::Phase phase);
    bool persistCommandIntent(const makcm::ota::Frame &request,
                              makcm::ota::State accepted_from,
                              makcm::ota::snapshot::Phase phase);
    bool persistCommandFact(const makcm::ota::Frame &request,
                            makcm::ota::snapshot::EffectPhase effect,
                            makcm::ota::snapshot::Phase phase,
                            makcm::ota::NackReason result,
                            makcm::ota::State resulting_state);
    bool finalizeCommandResponse(const makcm::ota::Frame &request,
                                 const makcm::ota::Frame &response,
                                 makcm::ota::NackReason result,
                                 bool accepted);
    void fillReplayResponse(const makcm::ota::Frame &request,
                            const makcm::ota::snapshot::LedgerEntry &entry,
                            makcm::ota::Frame &response) const;
    void fillTombstoneResponse(
        const makcm::ota::Frame &request,
        const makcm::ota::snapshot::CompletedTombstone &completed,
        makcm::ota::Frame &response) const;

    OtaUpdateConfig config_{};
    std::atomic<makcm::ota::State> state_{makcm::ota::State::Idle};
    std::atomic<bool> restart_requested_{false};
    nvs_handle_t nvs_ = 0;
    const esp_partition_t *update_partition_ = nullptr;
    esp_ota_handle_t ota_handle_ = 0;
    bool ota_open_ = false;
    bool manifest_valid_ = false;
    bool hash_open_ = false;
    makcm::ota::Manifest manifest_{};
    uint8_t manifest_wire_[makcm::ota::kManifestWireSize]{};
    mbedtls_sha256_context image_hash_{};
    uint32_t transaction_id_ = 0;
    uint32_t next_offset_ = 0;
    uint32_t last_data_offset_ = 0;
    uint16_t last_data_length_ = 0;
    uint32_t last_data_crc_ = 0;
    uint32_t last_data_sequence_ = 0;
    uint8_t last_data_sha256_[32]{};
    uint32_t last_activity_ms_ = 0;
    uint32_t pending_confirm_started_ms_ = 0;
    bool local_boot_validated_ = false;
    LocalRecord completed_record_{};
    bool completed_record_valid_ = false;
    makcm::ota::snapshot::Snapshot snapshot_{};
    makcm::ota::snapshot::Snapshot pending_snapshot_{};
    makcm::ota::snapshot::Snapshot snapshot_candidate_a_{};
    makcm::ota::snapshot::Snapshot snapshot_candidate_b_{};
    uint8_t snapshot_wire_a_[makcm::ota::snapshot::kSnapshotWireSize]{};
    uint8_t snapshot_wire_b_[makcm::ota::snapshot::kSnapshotWireSize]{};
    uint8_t snapshot_write_wire_[makcm::ota::snapshot::kSnapshotWireSize]{};
    uint8_t snapshot_readback_wire_[makcm::ota::snapshot::kSnapshotWireSize]{};
    int8_t selected_snapshot_copy_ = -1;
    bool snapshot_ready_ = false;
    SnapshotCommitFault snapshot_commit_fault_ = SnapshotCommitFault::None;
    bool snapshot_copy_error_ = false;
    uint32_t snapshot_crc32_ = 0;
    BootEvidence boot_evidence_{};
};

} // namespace makcm::native
