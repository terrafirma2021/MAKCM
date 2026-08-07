#pragma once

// Right-board maintenance receiver.  This interface is deliberately kept
// separate from the real-time bridge protocol.  The receiver is quiescent
// until the host sends the exact text command OTA_ENTER; while active it owns
// UART1 and consumes only MKOT binary frames.

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include "OtaManifest.h"
#include "OtaProtocol.h"
#include "OtaSnapshotV2.h"
#include "OtaStatusV2.h"
#include "esp_system.h"
#include "mbedtls/sha256.h"
#include <atomic>
#include <stddef.h>
#include <stdint.h>

class Preferences;

class OtaReceiver {
public:
    static constexpr uint8_t kProtocolVersion = makcm::ota::kWireVersion;
    static constexpr uint8_t kTargetRight =
        static_cast<uint8_t>(makcm::ota::Target::Right);
    static constexpr size_t kMaxChunk = makcm::ota::kMaxChunkSize;
    static constexpr size_t kMaxFrameSize = makcm::ota::kMaxWireSize;
    // Must exceed the host's full retry window (10s timeout x 3 transmissions
    // = 30s): if a DATA transmission is lost on the wire, the retransmit must
    // find the receiver still in Receiving, not tick-aborted to Failed.
    static constexpr uint32_t kIdleTimeoutMs = 45000;
    static constexpr uint32_t kConfirmTimeoutMs = 120000;
    using LocalHealthCheck = bool (*)(void *context);
    using PairHealthCheck = bool (*)(void *context);

    enum class State : uint8_t {
        Idle = 0,
        Receiving = 1,
        Prepared = 2,
        Activated = 3,
        AwaitConfirm = 4,
        Failed = 5,
        Confirmed = 6,
        ActivateIntent = 7,
    };

    enum Error : uint16_t {
        Ok = 0,
        BadFrame = 1,
        BadVersion = 2,
        BadTarget = 3,
        BadTransaction = 4,
        BadState = 5,
        BadManifest = 6,
        BadRole = 7,
        BadHardware = 8,
        BadPartition = 9,
        BadImage = 10,
        BadOffset = 11,
        BadCrc = 12,
        Timeout = 13,
        HashMismatch = 14,
        SignatureRequired = 15,
        RollbackUnavailable = 16,
        Flash = 17,
        Unsupported = 18,
    };

    // The manifest is little-endian and carries a required P-256 signature
    // verified against the compiled public trust anchor.
    void begin(HardwareSerial *serial, SemaphoreHandle_t *mutex,
               LocalHealthCheck localHealthCheck = nullptr,
               PairHealthCheck pairHealthCheck = nullptr,
               void *healthContext = nullptr);
    // Six distinct refusals, and the bootstrap line that reports them cannot
    // ask a follow-up question: the board is by definition not in maintenance,
    // so it cannot answer Status V2 either.  Name the site or the host is left
    // guessing which of them fired.
    enum class MaintenanceReject : uint8_t {
        None = 0,
        NotInitialized = 1,
        ZeroTransaction = 2,
        ActiveTransactionMismatch = 3,
        BoundTransactionMismatch = 4,
        BootstrapTransactionMismatch = 5,
        SerialBusy = 6,
        DisplacementEraseFailed = 7,
    };
    bool enterMaintenance(uint32_t transactionId = 0,
                          uint32_t peerGeneration = 0);
    MaintenanceReject lastMaintenanceReject() const {
        return lastMaintenanceReject_;
    }
    bool isMaintenanceActive() const {
        return maintenanceActive_.load(std::memory_order_acquire);
    }
    // Expose the MKOT decoder fill level so UART1 can keep text bootstrap
    // (OTA_MAINT_BEGIN) audible while exclusive maintenance owns binary frames.
    size_t decoderBuffered() const { return decoder_.buffered(); }
    void feed(uint8_t byte);
    void tick();
    bool sendText(const char *text);
    State state() const { return state_; }
    bool peerCompatible() const;
    bool healthPassed() const {
        return pairHealthCheck_ != nullptr && pairHealthCheck_(healthContext_);
    }
    bool localHealthPassed() const {
        return localHealthCheck_ != nullptr &&
               localHealthCheck_(healthContext_);
    }
    bool localBootValidated() const { return localBootValidated_; }

private:
    struct CompletedRecord {
        uint32_t magic = 0;
        uint16_t version = 0;
        uint8_t slot = 0xFF;
        uint8_t reserved = 0;
        uint32_t transactionId = 0;
        uint32_t generation = 0;
        uint32_t minPeerGeneration = 0;
        uint32_t maxPeerGeneration = 0;
        uint8_t imageSha256[32] = {};
        uint32_t crc = 0;
    };

    static constexpr uint32_t kCompletedMagic = 0x314E4F44u; // "DON1"
    static constexpr uint16_t kCompletedVersion = 1;
    Preferences *prefs_ = nullptr;
    Preferences *completedPrefs_ = nullptr;
    HardwareSerial *serial_ = nullptr;
    SemaphoreHandle_t *serialMutex_ = nullptr;
    LocalHealthCheck localHealthCheck_ = nullptr;
    PairHealthCheck pairHealthCheck_ = nullptr;
    void *healthContext_ = nullptr;
    std::atomic<bool> maintenanceActive_{false};
    State state_ = State::Idle;
    uint32_t transactionId_ = 0;
    uint32_t replyTransactionId_ = 1;
    uint32_t replySequence_ = 0;
    uint32_t bootstrapTransactionId_ = 0;
    uint32_t targetGeneration_ = 0;
    uint32_t peerGeneration_ = 0;
    MaintenanceReject lastMaintenanceReject_ = MaintenanceReject::None;
    uint32_t minPeerGeneration_ = 0;
    uint32_t maxPeerGeneration_ = 0;
    uint32_t imageSize_ = 0;
    uint32_t bytesWritten_ = 0;
    uint32_t lastError_ = Ok;
    uint32_t lastActivityMs_ = 0;
    uint8_t targetSubtype_ = 0xFF;
    uint8_t imageSha256_[32] = {};
    uint16_t signatureLength_ = 0;
    uint16_t manifestLength_ = 0;
    uint32_t lastSequence_ = 0;
    bool haveSequence_ = false;
    bool localBootValidated_ = false;
    CompletedRecord completed_{};
    bool completedValid_ = false;
    esp_err_t lastOtaError_ = ESP_OK;
    esp_err_t lastStateQueryError_ = ESP_OK;
    esp_ota_img_states_t lastImageState_ = ESP_OTA_IMG_UNDEFINED;
    esp_ota_handle_t otaHandle_ = 0;
    const esp_partition_t *targetPartition_ = nullptr;
    void resetParser();
    void handleFrame(const makcm::ota::Frame &frame);
    void sendResponse(makcm::ota::Command command, Error error, uint32_t offset = 0,
                      const uint8_t *extra = nullptr, uint16_t extraLength = 0);
    void sendStatus(const makcm::ota::Frame &request);
    bool fillStatusV2(const makcm::ota::Frame &request,
                      makcm::ota::Frame &response);
    bool abortUpdate(Error error, bool clearPersistent = true);
    bool persistState();
    bool persistCompletedState();
    bool loadCompletedState();
    bool completedMatches(uint32_t transactionId) const;
    static uint32_t completedCrc(const CompletedRecord &record);
    void loadPersistentState();
    bool loadSnapshotV2();
    bool readSnapshotCopy(const char *key, uint8_t *wire,
                          makcm::ota::snapshot::Snapshot &decoded,
                          bool &present, bool &valid);
    bool commitPendingSnapshotV2();
    makcm::ota::snapshot::Snapshot &beginSnapshotMutation();
    bool restoreFromSnapshotV2(const esp_partition_t *running,
                               const esp_partition_t *boot,
                               esp_ota_img_states_t imageState);
    const esp_partition_t *partitionForSubtype(uint8_t subtype) const;
    // True when the persisted active transaction installed nothing this board
    // will ever boot, so a new transaction may take its place.
    bool activeSnapshotIsDisplaceable() const;
    bool partitionMatchesImage(const esp_partition_t *partition,
                               const makcm::ota::snapshot::Identity &identity) const;
    bool partitionMatchesPreviousImage(
        const esp_partition_t *partition,
        const makcm::ota::snapshot::Identity &identity) const;
    bool identityDigestMatches(
        const makcm::ota::snapshot::Identity &identity) const;
    bool persistLocalValidationFact();
    bool buildActiveIdentity(const makcm::ota::Frame &manifestRequest);
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
                                 Error &error);
    bool setLedgerIntent(makcm::ota::snapshot::ActiveTransaction &active,
                         const makcm::ota::Frame &request,
                         makcm::ota::State acceptedFrom,
                         makcm::ota::snapshot::Phase phase);
    bool persistCommandIntent(const makcm::ota::Frame &request,
                              makcm::ota::State acceptedFrom,
                              makcm::ota::snapshot::Phase phase);
    bool persistCommandFact(const makcm::ota::Frame &request,
                            makcm::ota::snapshot::EffectPhase effect,
                            makcm::ota::snapshot::Phase phase,
                            makcm::ota::NackReason result,
                            makcm::ota::State resultingState);
    bool persistFactOrFail(const makcm::ota::Frame &request,
                           makcm::ota::snapshot::EffectPhase effect,
                           makcm::ota::snapshot::Phase phase,
                           makcm::ota::NackReason result,
                           makcm::ota::State resultingState,
                           Error &error);
    bool finalizeCommandResponse(const makcm::ota::Frame &request,
                                 const makcm::ota::Frame &response,
                                 makcm::ota::NackReason result,
                                 bool accepted);
    void fillResponse(const makcm::ota::Frame &request,
                      makcm::ota::Command responseCommand,
                      makcm::ota::NackReason reason,
                      makcm::ota::Frame &response) const;
    void fillReplayResponse(const makcm::ota::Frame &request,
                            const makcm::ota::snapshot::LedgerEntry &entry,
                            makcm::ota::Frame &response) const;
    void fillTombstoneResponse(
        const makcm::ota::Frame &request,
        const makcm::ota::snapshot::CompletedTombstone &completed,
        makcm::ota::Frame &response) const;
    bool validateManifest(const uint8_t *payload, uint16_t length,
                          Error &error);
    bool verifySignature(const uint8_t *manifest, uint16_t length) const;
    bool prepareImage(const makcm::ota::Frame &request, Error &error);
    bool handleData(uint32_t sequence, uint32_t offset,
                    const uint8_t *payload, uint16_t length, Error &error);
    bool handleActivate(const makcm::ota::Frame &request, Error &error);
    bool handleConfirm(const makcm::ota::Frame &request, Error &error);
    bool validateLocalBoot();
    bool writeWire(const uint8_t *data, size_t length);
    makcm::ota::StreamDecoder decoder_;
    makcm::ota::Frame requestFrame_{};
    makcm::ota::Frame responseFrame_{};
    uint8_t wireBuffer_[kMaxFrameSize] = {};
    makcm::ota::Manifest manifest_{};
    uint8_t manifestWire_[makcm::ota::kManifestWireSize] = {};
    mbedtls_sha256_context imageHash_{};
    bool hashOpen_ = false;
    uint32_t lastDataOffset_ = 0;
    uint16_t lastDataLength_ = 0;
    uint32_t lastDataCrc_ = 0;
    uint32_t lastDataSequence_ = 0;
    uint8_t lastDataSha256_[32] = {};
    esp_reset_reason_t resetReason_ = ESP_RST_UNKNOWN;
    esp_err_t lastNvsError_ = ESP_OK;
    bool snapshotCopyError_ = false;
    esp_err_t markValidError_ = ESP_OK;
    makcm::ota::snapshot::Snapshot snapshot_{};
    makcm::ota::snapshot::Snapshot pendingSnapshot_{};
    makcm::ota::snapshot::Snapshot snapshotCandidateA_{};
    makcm::ota::snapshot::Snapshot snapshotCandidateB_{};
    uint8_t snapshotWireA_[makcm::ota::snapshot::kSnapshotWireSize] = {};
    uint8_t snapshotWireB_[makcm::ota::snapshot::kSnapshotWireSize] = {};
    uint8_t snapshotWriteWire_[makcm::ota::snapshot::kSnapshotWireSize] = {};
    uint8_t snapshotReadbackWire_[makcm::ota::snapshot::kSnapshotWireSize] = {};
    uint8_t statusV2Wire_[makcm::ota::kStatusV2PayloadSize] = {};
    int8_t selectedSnapshotCopy_ = -1;
    bool snapshotReady_ = false;
};

extern OtaReceiver otaReceiver;
