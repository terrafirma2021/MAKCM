#include "OtaReceiver.h"

#include <Preferences.h>
#include <algorithm>
#include <esp_app_format.h>
#include <esp_log.h>
#include <esp_partition.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/ecp.h>
#include <mbedtls/sha256.h>
#include <mbedtls/version.h>
#include <string.h>

#include "OtaBoardConfig.h"
#include "OtaGeneration.h"
#include "OtaImageIdentity.h"
#include "OtaTrustAnchor.h"

namespace {
constexpr char kTag[] = "makcm_ota_right";
constexpr char kCompletedNamespace[] = "makcm_ota_done";
constexpr char kCompletedKey[] = "record";
constexpr char kSnapshotAKey[] = "snap_a";
constexpr char kSnapshotBKey[] = "snap_b";
constexpr uint32_t kCurrentGeneration = makcm::ota::build::kPairGeneration;
constexpr size_t kManifestLength = makcm::ota::kManifestWireSize;
static uint8_t s_compareBuffer[makcm::ota::kMaxChunkSize] = {};

bool exactFixedString(const char *expected, const char *actual,
                      size_t capacity);

// Arduino's app descriptor is supplied by a prebuilt framework and cannot
// carry the pair generation/role/schema contract.  Keep a fixed marker in the
// application image and verify the copy read back from the OTA slot before it
// becomes PREPARED.
const makcm::ota::EmbeddedImageIdentity g_makcm_right_image_identity
    __attribute__((used, section(".rodata.makcm_identity"))) = {
    {'M', 'K', 'I', 'M', 'G', 'V', '1', '\0'},
    makcm::ota::kEmbeddedIdentityVersion,
    static_cast<uint16_t>(sizeof(makcm::ota::EmbeddedImageIdentity)),
    static_cast<uint8_t>(makcm::ota::Target::Right),
    {0, 0, 0},
    makcm::ota::build::kPairGeneration,
    makcm::ota::board::kRightPartitionSchemaV1,
    makcm::ota::board::kPairProtocolVersion,
    {0, 0},
    "makcm-s3-r1",
    "makcm_right_arduino",
    MAKCM_OTA_PAIR_APP_VERSION,
    {'I', 'D', 'E', 'N', 'D', 'V', '1', '\0'},
};

bool markerLinked() {
    // Xtensa GCC 8 ignores `retain`; volatile reads from the startup path
    // create a real relocation so --gc-sections cannot discard the marker.
    volatile const uint8_t *raw = reinterpret_cast<volatile const uint8_t *>(
        &g_makcm_right_image_identity);
    makcm::ota::EmbeddedImageIdentity copy{};
    for (size_t i = 0; i < sizeof(copy); ++i)
        reinterpret_cast<uint8_t *>(&copy)[i] = raw[i];
    return makcm::ota::validEmbeddedImageIdentity(copy) &&
           copy.role == static_cast<uint8_t>(makcm::ota::Target::Right) &&
           copy.generation == makcm::ota::build::kPairGeneration &&
           copy.partition_schema == makcm::ota::board::kRightPartitionSchemaV1;
}

bool verifyEmbeddedIdentity(const esp_partition_t *partition,
                            uint32_t imageSize,
                            const makcm::ota::Manifest &manifest) {
    using namespace makcm::ota;
    if (partition == nullptr || imageSize < sizeof(EmbeddedImageIdentity) ||
        imageSize > partition->size) return false;
    const size_t step = sizeof(EmbeddedImageIdentity) - 1;
    uint8_t found = 0;
    for (uint32_t base = 0; base < imageSize;) {
        const uint32_t count = std::min<uint32_t>(
            sizeof(s_compareBuffer), imageSize - base);
        if (esp_partition_read(partition, base, s_compareBuffer, count) !=
            ESP_OK) return false;
        for (uint32_t i = 0; i + sizeof(EmbeddedImageIdentity) <= count; ++i) {
            EmbeddedImageIdentity identity{};
            memcpy(&identity, s_compareBuffer + i, sizeof(identity));
            if (!validEmbeddedImageIdentity(identity)) continue;
            char expectedVersion[32] = {};
            snprintf(expectedVersion, sizeof(expectedVersion), "makcm-g%lu",
                     static_cast<unsigned long>(manifest.generation));
            if (identity.role != static_cast<uint8_t>(manifest.role) ||
                identity.generation != manifest.generation ||
                identity.partition_schema != manifest.partition_schema ||
                identity.protocol_version != manifest.protocol_version ||
                !exactFixedString(board::kHardwareRevisionR1,
                                  identity.hardware_revision,
                                  sizeof(identity.hardware_revision)) ||
                !exactFixedString(board::kRightManifestProjectName,
                                  identity.project_name,
                                  sizeof(identity.project_name)) ||
                !exactFixedString(expectedVersion, identity.app_version,
                                  sizeof(identity.app_version))) continue;
            if (++found > 1) return false;
        }
        if (count == imageSize - base) break;
        base += count > step ? count - static_cast<uint32_t>(step) : 1;
    }
    return found == 1;
}

bool exactFixedString(const char *expected, const char *actual,
                      size_t capacity) {
    if (expected == nullptr || actual == nullptr || capacity < 2) return false;
    const size_t length = strlen(expected);
    return length != 0 && length < capacity &&
           memcmp(actual, expected, length) == 0 &&
           actual[length] == '\0';
}

bool allZero(const uint8_t *data, size_t length) {
    uint8_t aggregate = 0;
    for (size_t i = 0; i < length; ++i) aggregate |= data[i];
    return aggregate == 0;
}

bool isOtaApplication(const esp_partition_t *partition) {
    return partition != nullptr &&
           partition->type == ESP_PARTITION_TYPE_APP &&
           (partition->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 ||
            partition->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1);
}

bool looksLikeMergedImage(const esp_partition_t *partition, uint32_t imageSize) {
    if (partition == nullptr) return true;
    uint8_t bytes[4] = {};
    for (const uint32_t offset : {0x8000u, 0x9000u}) {
        if (offset + 2 > imageSize ||
            esp_partition_read(partition, offset, bytes, 2) != ESP_OK)
            continue;
        if (bytes[0] == 0xAA && bytes[1] == 0x50) return true;
    }
    // Do not reject a valid application merely because arbitrary payload at
    // 0x10000 happens to start with the ESP image byte 0xE9 (a 1/256 false
    // positive).  The signed manifest, full-image SHA, application header,
    // descriptor and embedded role/generation identity are authoritative.
    return false;
}

makcm::ota::NackReason toNack(OtaReceiver::Error error) {
    using makcm::ota::NackReason;
    switch (error) {
    case OtaReceiver::Ok: return NackReason::None;
    case OtaReceiver::BadFrame: return NackReason::Malformed;
    case OtaReceiver::BadVersion: return NackReason::UnsupportedVersion;
    case OtaReceiver::BadTarget: return NackReason::WrongTarget;
    case OtaReceiver::BadTransaction: return NackReason::WrongTransaction;
    case OtaReceiver::BadState: return NackReason::InvalidState;
    // These five were one wire code, which made a manifest the host could
    // verify itself indistinguishable from a board-side fact the host cannot
    // see at all.  The two that matter have accurate names already: BadRole is
    // a manifest addressed to the wrong participant, and BadHardware covers
    // hardware revision, project name, generation monotonicity and -- the one
    // piece of state only the board knows -- whether it has learned its peer's
    // generation yet.  The remaining three describe the manifest's own bytes,
    // which the host holds and can re-check.
    case OtaReceiver::BadRole: return NackReason::WrongTarget;
    case OtaReceiver::BadHardware: return NackReason::CompatibilityRejected;
    case OtaReceiver::BadManifest:
    case OtaReceiver::BadPartition:
    case OtaReceiver::BadImage:
        return NackReason::ManifestRejected;
    case OtaReceiver::BadOffset: return NackReason::WrongOffset;
    case OtaReceiver::BadCrc: return NackReason::CrcMismatch;
    case OtaReceiver::Timeout: return NackReason::Timeout;
    case OtaReceiver::HashMismatch: return NackReason::ImageVerificationFailed;
    case OtaReceiver::SignatureRequired:
        return NackReason::SignatureVerificationFailed;
    case OtaReceiver::RollbackUnavailable:
        return NackReason::RollbackUnavailable;
    case OtaReceiver::Flash: return NackReason::FlashWriteFailed;
    case OtaReceiver::Unsupported: return NackReason::InvalidState;
    }
    return NackReason::Malformed;
}

makcm::ota::State toCommonState(OtaReceiver::State state, bool maintenance) {
    using makcm::ota::State;
    if (maintenance && state == OtaReceiver::State::Idle) return State::Maintenance;
    switch (state) {
    case OtaReceiver::State::Idle: return State::Idle;
    case OtaReceiver::State::Receiving: return State::Receiving;
    case OtaReceiver::State::Prepared: return State::Prepared;
    case OtaReceiver::State::Activated: return State::ActivationPending;
    case OtaReceiver::State::AwaitConfirm: return State::PendingConfirm;
    case OtaReceiver::State::Failed: return State::Fault;
    case OtaReceiver::State::Confirmed: return State::Confirmed;
    case OtaReceiver::State::ActivateIntent: return State::ActivationPending;
    }
    return State::Fault;
}

uint32_t statusFlags(const OtaReceiver *receiver) {
    uint32_t flags = receiver->isMaintenanceActive()
                         ? makcm::ota::kStatusMaintenanceExclusive
                         : 0;
    if (receiver->state() == OtaReceiver::State::Prepared)
        flags |= makcm::ota::kStatusPrepared;
    if (receiver->state() == OtaReceiver::State::Activated ||
        receiver->state() == OtaReceiver::State::ActivateIntent ||
        receiver->state() == OtaReceiver::State::AwaitConfirm)
        flags |= makcm::ota::kStatusPendingVerify;
    if (receiver->peerCompatible())
        flags |= makcm::ota::kStatusPeerCompatible;
    if (receiver->healthPassed())
        flags |= makcm::ota::kStatusHealthPassed;
    if (receiver->localBootValidated())
        flags |= makcm::ota::kStatusLocalBootValidated;
#if defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE) && CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    flags |= makcm::ota::kStatusRollbackSupported;
#endif
    if (makcm::ota::trust::configured())
        flags |= makcm::ota::kStatusTrustConfigured;
    return flags;
}

bool verifyP256(const uint8_t digest[32], const uint8_t signature[64]) {
    using namespace makcm::ota::trust;
    if (!configured()) return false;
    mbedtls_ecdsa_context context;
    mbedtls_mpi r;
    mbedtls_mpi s;
    mbedtls_ecdsa_init(&context);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);
#if MBEDTLS_VERSION_MAJOR >= 3
    auto &group = context.MBEDTLS_PRIVATE(grp);
    auto &point = context.MBEDTLS_PRIVATE(Q);
    auto &point_x = point.MBEDTLS_PRIVATE(X);
    auto &point_y = point.MBEDTLS_PRIVATE(Y);
    auto &point_z = point.MBEDTLS_PRIVATE(Z);
#else
    auto &group = context.grp;
    auto &point = context.Q;
    auto &point_x = point.X;
    auto &point_y = point.Y;
    auto &point_z = point.Z;
#endif
    bool ok = mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1) == 0;
    ok = ok && mbedtls_mpi_read_binary(&point_x, kPublicX, sizeof(kPublicX)) == 0;
    ok = ok && mbedtls_mpi_read_binary(&point_y, kPublicY, sizeof(kPublicY)) == 0;
    ok = ok && mbedtls_mpi_lset(&point_z, 1) == 0;
    ok = ok && mbedtls_mpi_read_binary(&r, signature, 32) == 0;
    ok = ok && mbedtls_mpi_read_binary(&s, signature + 32, 32) == 0;
    ok = ok && mbedtls_ecdsa_verify(&group, digest, 32, &point, &r, &s) == 0;
    mbedtls_mpi_free(&s);
    mbedtls_mpi_free(&r);
    mbedtls_ecdsa_free(&context);
    return ok;
}
}

OtaReceiver otaReceiver;

void OtaReceiver::begin(HardwareSerial *serial, SemaphoreHandle_t *mutex,
                        LocalHealthCheck localHealthCheck,
                        PairHealthCheck pairHealthCheck,
                        void *healthContext) {
    serial_ = serial;
    serialMutex_ = mutex;
    localHealthCheck_ = localHealthCheck;
    pairHealthCheck_ = pairHealthCheck;
    healthContext_ = healthContext;
    maintenanceActive_ = false;
    decoder_.reset();
    resetReason_ = esp_reset_reason();
    mbedtls_sha256_init(&imageHash_);
    if (serial_ == nullptr || serialMutex_ == nullptr ||
        *serialMutex_ == nullptr) {
        state_ = State::Failed;
        lastError_ = Flash;
        maintenanceActive_.store(true, std::memory_order_release);
        return;
    }
    if (!markerLinked()) {
        state_ = State::Failed;
        lastError_ = BadImage;
        maintenanceActive_.store(true, std::memory_order_release);
        return;
    }
    if (prefs_ == nullptr) prefs_ = new (std::nothrow) Preferences();
    if (completedPrefs_ == nullptr)
        completedPrefs_ = new (std::nothrow) Preferences();
    if (prefs_ == nullptr || completedPrefs_ == nullptr ||
        !prefs_->begin("makcm_ota_r", false) ||
        !completedPrefs_->begin(kCompletedNamespace, false)) {
        ESP_LOGE(kTag, "NVS unavailable; OTA remains fail-closed");
        delete prefs_;
        prefs_ = nullptr;
        delete completedPrefs_;
        completedPrefs_ = nullptr;
        state_ = State::Failed;
        lastError_ = Flash;
        maintenanceActive_.store(true, std::memory_order_release);
        return;
    }
    if (!loadSnapshotV2()) {
        state_ = State::Failed;
        lastError_ = Flash;
        maintenanceActive_.store(true, std::memory_order_release);
        return;
    }
    loadPersistentState();
    if (state_ == State::Failed)
        maintenanceActive_.store(true, std::memory_order_release);
    lastActivityMs_ = millis();
}

// The persisted active transaction may be displaced when this board will never
// boot what it wrote.  boot != target is the whole claim: whatever phase the
// record reached, an image the bootloader will not select installed nothing.
// Checking the phase instead would be weaker -- a phase is what the board
// intended, the boot partition is what will actually happen.
bool OtaReceiver::activeSnapshotIsDisplaceable() const {
    if (!snapshot_.active.present) return true;
    const esp_partition_t *target =
        partitionForSubtype(snapshot_.active.identity.target_slot);
    if (target == nullptr) return true;
    return esp_ota_get_boot_partition() != target;
}

bool OtaReceiver::enterMaintenance(uint32_t transactionId,
                                   uint32_t peerGeneration) {
    lastMaintenanceReject_ = MaintenanceReject::None;
    if (serial_ == nullptr || serialMutex_ == nullptr || prefs_ == nullptr) {
        lastMaintenanceReject_ = MaintenanceReject::NotInitialized;
        return false;
    }
    // A dead transaction used to own this board forever.  loadPersistentState()
    // restores transactionId_ from the persisted active snapshot at every boot,
    // and both guards below then rejected every transaction that was not that
    // one -- with no displacement rule anywhere, unlike the left board.  An
    // interrupted download therefore made the board refuse all future updates,
    // and the only exit was a BOOT cable.
    const bool displaceable = activeSnapshotIsDisplaceable();
    if (snapshot_.active.present) {
        const uint32_t activeTransaction =
            snapshot_.active.identity.transaction_id;
        if (transactionId == 0) {
            lastMaintenanceReject_ = MaintenanceReject::ZeroTransaction;
            return false;
        }
        if (transactionId != activeTransaction && !displaceable) {
            lastMaintenanceReject_ =
                MaintenanceReject::ActiveTransactionMismatch;
            return false;
        }
    }
    if (transactionId_ != 0 && transactionId != transactionId_ &&
        !displaceable) {
        lastMaintenanceReject_ = MaintenanceReject::BoundTransactionMismatch;
        return false;
    }
    // The volatile bootstrap binding needs the same displacement rule as the
    // two durable bindings above.  A bootstrap that latched maintenance but
    // died before Manifest leaves no active record, so `displaceable` is true
    // and rejecting here would hand the board to a dead transaction until a
    // power cycle -- every host-side ENTER failure then costs a physical
    // replug before the next attempt can even bootstrap.
    if (maintenanceActive_.load(std::memory_order_acquire) &&
        bootstrapTransactionId_ != 0 &&
        transactionId != bootstrapTransactionId_ && !displaceable) {
        lastMaintenanceReject_ =
            MaintenanceReject::BootstrapTransactionMismatch;
        return false;
    }
    // Displacement must also erase the dead transaction's durable active
    // record.  Clearing only the volatile binding grants maintenance, but the
    // Manifest/Abort transaction gates keep judging every frame against the
    // stale persisted identity, so the new transaction is NACKed
    // wrong_transaction right after READY and the record can never be
    // aborted either.
    if (snapshot_.active.present && displaceable && transactionId != 0 &&
        transactionId != snapshot_.active.identity.transaction_id) {
        auto &next = beginSnapshotMutation();
        memset(&next.active, 0, sizeof(next.active));
        if (!commitPendingSnapshotV2()) {
            lastMaintenanceReject_ =
                MaintenanceReject::DisplacementEraseFailed;
            return false;
        }
        state_ = State::Idle;
    }
    if (xSemaphoreTake(*serialMutex_, pdMS_TO_TICKS(100)) != pdTRUE) {
        lastMaintenanceReject_ = MaintenanceReject::SerialBusy;
        return false;
    }
    // Taking over from a displaced transaction must not leave its id bound, or
    // the very next guard rejects the transaction we just accepted.
    if (transactionId != 0 && transactionId != transactionId_ && displaceable)
        transactionId_ = 0;
    maintenanceActive_.store(true, std::memory_order_release);
    if (transactionId != 0) {
        bootstrapTransactionId_ = transactionId;
        replyTransactionId_ = transactionId;
    }
    if (peerGeneration != 0) peerGeneration_ = peerGeneration;
    lastActivityMs_ = millis();
    decoder_.reset();
    xSemaphoreGive(*serialMutex_);
    return true;
}

bool OtaReceiver::peerCompatible() const {
    if (peerGeneration_ == 0) return false;
    if (state_ == State::Idle) return peerGeneration_ == kCurrentGeneration;
    return peerGeneration_ >= minPeerGeneration_ &&
           peerGeneration_ <= maxPeerGeneration_;
}

bool OtaReceiver::sendText(const char *text) {
    if (text == nullptr) return false;
    return writeWire(reinterpret_cast<const uint8_t *>(text), strlen(text));
}

void OtaReceiver::resetParser() { decoder_.reset(); }

bool OtaReceiver::writeWire(const uint8_t *data, size_t length) {
    if (serial_ == nullptr || data == nullptr || length == 0) return false;
    if (serialMutex_ != nullptr &&
        xSemaphoreTake(*serialMutex_, pdMS_TO_TICKS(100)) != pdTRUE)
        return false;
    const size_t written = serial_->write(data, length);
    if (serialMutex_ != nullptr) xSemaphoreGive(*serialMutex_);
    return written == length;
}

void OtaReceiver::fillResponse(const makcm::ota::Frame &request,
                               makcm::ota::Command responseCommand,
                               makcm::ota::NackReason reason,
                               makcm::ota::Frame &response) const {
    using namespace makcm::ota;
    memset(&response, 0, sizeof(response));
    response.target = Target::Right;
    response.command = responseCommand;
    response.flags = kFlagResponse;
    response.transaction_id = request.transaction_id != 0
        ? request.transaction_id : transactionId_;
    response.offset = bytesWritten_;
    response.sequence = request.sequence;
    StatusPayload status{};
    status.state = toCommonState(state_, maintenanceActive_);
    status.last_command = request.command;
    status.reason = reason;
    status.next_offset = bytesWritten_;
    status.target_generation = targetGeneration_;
    status.active_generation = kCurrentGeneration;
    status.status_flags = statusFlags(this);
    encodeStatusPayload(status, response.payload);
    response.length = kStatusPayloadSize;
}

void OtaReceiver::sendResponse(makcm::ota::Command command, Error error,
                               uint32_t offset, const uint8_t *extra,
                               uint16_t extraLength) {
    using namespace makcm::ota;
    (void)offset;
    (void)extra;
    (void)extraLength;
    fillResponse(requestFrame_, error == Ok ? Command::Ack : Command::Nack,
                 toNack(error), responseFrame_);
    responseFrame_.transaction_id = transactionId_ != 0 ? transactionId_
                                                          : replyTransactionId_;
    responseFrame_.sequence = replySequence_ == 0 ? 1 : replySequence_;
    responseFrame_.offset = bytesWritten_;
    /*
     * Preserve the historical StatusPayload wire shape.  The V2 snapshot is
     * the durable authority; v1 status remains the compatibility surface.
     */
    const size_t wireLength = encodeFrame(responseFrame_, wireBuffer_,
                                          sizeof(wireBuffer_));
    if (wireLength != 0) writeWire(wireBuffer_, wireLength);
}

bool OtaReceiver::fillStatusV2(const makcm::ota::Frame &request,
                               makcm::ota::Frame &response) {
    using namespace makcm::ota;
    using namespace makcm::ota::snapshot;
    if (!snapshotReady_ || request.command != Command::Status ||
        (request.flags & kFlagStatusV2) == 0 || request.length != 0)
        return false;

    StatusPayloadV2 status{};
    status.role = Target::Right;
    status.protocol_epoch = kProtocolEpoch;
    // The generation of the image currently executing.  This is a property of
    // the running build, not of any snapshot record, so it must be reported in
    // every state -- including a board with no active transaction and no
    // tombstone.  Leaving it zero made the host read a healthy right board as
    // "peer generation 0" and refuse the rolling-activation compatibility
    // check, which no amount of snapshot repair could fix.
    status.active_generation = kCurrentGeneration;
    // These two fields correlate the Status V2 payload with this exact
    // request/response.  Durable lifecycle progress is reported separately
    // through last_ota_operation, local_phase and the completed tombstone.
    status.last_command = request.command;
    status.status_flags = statusFlags(this);
    status.record_sequence = snapshot_.commit_sequence;
    status.reset_reason = static_cast<uint32_t>(resetReason_);
    status.last_ota_error = lastOtaError_;
    status.last_state_query_error = lastStateQueryError_;
    status.last_response_command = Command::StatusResponse;
    // These strings describe the image currently executing on the right
    // board, not a host-supplied manifest.  begin() has already validated the
    // embedded marker, so use its fixed values for every status state.
    memcpy(status.hardware_revision,
           g_makcm_right_image_identity.hardware_revision,
           sizeof(status.hardware_revision));
    memcpy(status.project_name, g_makcm_right_image_identity.project_name,
           sizeof(status.project_name));
    memcpy(status.app_version, g_makcm_right_image_identity.app_version,
           sizeof(status.app_version));

    const ActiveTransaction *active = snapshot_.active.present
        ? &snapshot_.active : nullptr;
    const CompletedTombstone *completed = snapshot_.completed.present
        ? &snapshot_.completed : nullptr;
    const Identity *identity = active != nullptr ? &active->identity
                            : completed != nullptr ? &completed->identity
                            : nullptr;

    if (active != nullptr) {
        status.local_phase = active->phase;
        status.state = toCommonState(state_, maintenanceActive_);
        status.last_ota_operation = active->last_ota_operation;
        status.reason = active->last_nack;
        status.next_offset = active->next_offset;
        status.record_transaction_id = identity->transaction_id;
        status.target_generation = identity->generation;
        status.previous_generation = identity->previous_generation;
        status.partition_schema = identity->partition_schema;
        status.image_size = identity->image_size;
        status.last_request_sequence = active->last_request_sequence;
        status.reset_reason = active->reset_reason;
        status.last_ota_error = active->last_ota_error;
        status.last_state_query_error = active->last_state_error;
        status.boot_attempt = active->boot_attempt;
        status.local_phase = active->phase;
        status.running_slot = active->running_slot;
        status.boot_slot = active->boot_slot;
        status.image_state = active->image_state;
        status.target_slot = identity->target_slot;
        status.previous_slot = identity->previous_slot;
        memcpy(status.image_sha256, identity->image_sha256,
               sizeof(status.image_sha256));
        memcpy(status.manifest_sha256, identity->manifest_sha256,
               sizeof(status.manifest_sha256));
        memcpy(status.identity_digest, identity->identity_digest,
               sizeof(status.identity_digest));
        memcpy(status.previous_image_sha256, identity->previous_image_sha256,
               sizeof(status.previous_image_sha256));
        memcpy(status.hardware_revision, identity->hardware_revision,
               sizeof(status.hardware_revision));
        memcpy(status.project_name, identity->project_name,
               sizeof(status.project_name));
        if (active->local_validated)
            status.evidence_flags |= kEvidenceLocalValidated;
        if (active->pair_committed)
            status.evidence_flags |= kEvidencePairCommitted;
    } else if (completed != nullptr) {
        // A completed tombstone is reported separately from the live
        // transaction.  The V2 validator requires local_phase=Empty whenever
        // no active transaction is present; outcome/terminal_command carry
        // the durable completion fact.
        status.local_phase = Phase::Empty;
        status.state = completed->outcome == Outcome::Confirmed
            ? makcm::ota::State::Confirmed : makcm::ota::State::Idle;
        status.last_ota_operation =
            static_cast<uint8_t>(completed->terminal_command);
        status.completed_outcome = completed->outcome;
        status.reason = completed->result;
        status.completed_transaction_id = identity->transaction_id;
        status.completed_terminal_sequence = completed->terminal_sequence;
        status.target_generation = identity->generation;
        status.previous_generation = identity->previous_generation;
        status.partition_schema = identity->partition_schema;
        status.image_size = identity->image_size;
        status.running_slot = completed->running_slot;
        status.boot_slot = completed->boot_slot;
        status.image_state = completed->image_state;
        status.completed_terminal_command = completed->terminal_command;
        memcpy(status.completed_image_sha256, identity->image_sha256,
               sizeof(status.completed_image_sha256));
        memcpy(status.hardware_revision, identity->hardware_revision,
               sizeof(status.hardware_revision));
        memcpy(status.project_name, identity->project_name,
               sizeof(status.project_name));
        if (completed->local_validated)
            status.evidence_flags |= kEvidenceLocalValidated;
        if (completed->pair_committed)
            status.evidence_flags |= kEvidencePairCommitted;
    } else {
        status.local_phase = Phase::Empty;
        status.state = toCommonState(state_, maintenanceActive_);
        status.reason = toNack(static_cast<Error>(lastError_));
        status.running_slot = esp_ota_get_running_partition() == nullptr
            ? 0xFF : static_cast<uint8_t>(
                esp_ota_get_running_partition()->subtype);
        status.boot_slot = esp_ota_get_boot_partition() == nullptr
            ? 0xFF : static_cast<uint8_t>(esp_ota_get_boot_partition()->subtype);
        memcpy(status.hardware_revision, makcm::ota::board::kHardwareRevisionR1,
               sizeof(status.hardware_revision));
        memcpy(status.project_name,
               makcm::ota::board::kRightManifestProjectName,
               sizeof(status.project_name));
    }
    if (completed != nullptr) {
        status.completed_transaction_id = identity->transaction_id;
        status.completed_outcome = completed->outcome;
        status.completed_terminal_sequence = completed->terminal_sequence;
        status.completed_terminal_command = completed->terminal_command;
        status.evidence_flags |= kEvidenceCompletedPresent;
        memcpy(status.completed_image_sha256, identity->image_sha256,
               sizeof(status.completed_image_sha256));
    }
    if (active != nullptr) status.evidence_flags |= kEvidenceActivePresent;
    if (snapshotReady_) status.evidence_flags |= kEvidenceSnapshotValid;
    if (snapshotCopyError_)
        status.evidence_flags |= kEvidenceSnapshotCopyError;
    if (identity != nullptr) {
        status.record_transaction_id = active != nullptr ? identity->transaction_id : 0;
        if (active != nullptr) {
            status.target_generation = identity->generation;
            status.previous_generation = identity->previous_generation;
            status.partition_schema = identity->partition_schema;
            status.image_size = identity->image_size;
        }
        status.target_slot = identity->target_slot;
        status.previous_slot = identity->previous_slot;
        if (identityDigestMatches(*identity))
            status.evidence_flags |= kEvidenceIdentityDigestValid;
    }
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    // Slots/state are live boot evidence.  Do not report stale values cached
    // in a tombstone or active snapshot after a reset/re-enumeration.
    status.running_slot = running == nullptr
        ? 0xFF : static_cast<uint8_t>(running->subtype);
    status.boot_slot = boot == nullptr
        ? 0xFF : static_cast<uint8_t>(boot->subtype);
    esp_ota_img_states_t currentImageState = ESP_OTA_IMG_UNDEFINED;
    const esp_err_t currentStateError = running == nullptr
        ? ESP_ERR_NOT_FOUND
        : esp_ota_get_state_partition(running, &currentImageState);
    status.last_state_query_error = static_cast<int32_t>(currentStateError);
    status.image_state = currentStateError == ESP_OK
        ? static_cast<uint8_t>(currentImageState) : 0xFF;
    if (running != nullptr) {
        uint8_t runningHash[32] = {};
        // This evidence bit means only that the immutable running-partition
        // hash was read successfully.  Whether it matches an OTA identity is
        // intentionally left to the host/pair verifier.
        if (esp_partition_get_sha256(running, runningHash) == ESP_OK) {
            memcpy(status.running_image_sha256, runningHash,
                   sizeof(status.running_image_sha256));
            status.evidence_flags |= kEvidenceRunningImageHashValid;
        }
        esp_app_desc_t descriptor{};
        if (esp_ota_get_partition_description(running, &descriptor) == ESP_OK)
            memcpy(status.app_elf_sha256, descriptor.app_elf_sha256,
                   sizeof(status.app_elf_sha256));
    }
    if (active != nullptr) {
        memcpy(status.image_sha256, active->identity.image_sha256,
               sizeof(status.image_sha256));
        memcpy(status.manifest_sha256, active->identity.manifest_sha256,
               sizeof(status.manifest_sha256));
        memcpy(status.identity_digest, active->identity.identity_digest,
               sizeof(status.identity_digest));
        memcpy(status.previous_image_sha256,
               active->identity.previous_image_sha256,
               sizeof(status.previous_image_sha256));
    }
    if (completed != nullptr) {
        memcpy(status.image_sha256, completed->identity.image_sha256,
               sizeof(status.image_sha256));
        memcpy(status.manifest_sha256, completed->identity.manifest_sha256,
               sizeof(status.manifest_sha256));
        memcpy(status.identity_digest, completed->identity.identity_digest,
               sizeof(status.identity_digest));
        memcpy(status.previous_image_sha256,
               completed->identity.previous_image_sha256,
               sizeof(status.previous_image_sha256));
    }
    if (selectedSnapshotCopy_ == 0)
        status.record_crc32 = readLe32(snapshotWireA_ + kCrcOffset);
    else if (selectedSnapshotCopy_ == 1)
        status.record_crc32 = readLe32(snapshotWireB_ + kCrcOffset);
    // Keep the running-image marker authoritative even when an active or
    // completed identity is also present in the snapshot.
    memcpy(status.hardware_revision,
           g_makcm_right_image_identity.hardware_revision,
           sizeof(status.hardware_revision));
    memcpy(status.project_name, g_makcm_right_image_identity.project_name,
           sizeof(status.project_name));
    memcpy(status.app_version, g_makcm_right_image_identity.app_version,
           sizeof(status.app_version));
    if (!encodeStatusV2(status, statusV2Wire_, sizeof(statusV2Wire_)))
        return false;
    memset(&response, 0, sizeof(response));
    response.target = Target::Right;
    response.command = Command::StatusResponse;
    response.flags = kFlagResponse | kFlagStatusV2;
    response.transaction_id = request.transaction_id;
    response.sequence = request.sequence;
    response.length = kStatusV2PayloadSize;
    response.offset = status.next_offset;
    memcpy(response.payload, statusV2Wire_, kStatusV2PayloadSize);
    return true;
}

void OtaReceiver::sendStatus(const makcm::ota::Frame &request) {
    using namespace makcm::ota;
    if ((request.flags & kFlagStatusV2) != 0) {
        if (request.command != Command::Status ||
            !fillStatusV2(request, responseFrame_)) {
            state_ = State::Failed;
            lastError_ = Flash;
            // Status is not a lifecycle command, so its encode failure cannot
            // create a ledger entry.  Fail closed and return a v1 NACK.
            sendResponse(request.command, Flash, request.offset);
            return;
        }
        const size_t v2Length = encodeFrame(responseFrame_, wireBuffer_,
                                            sizeof(wireBuffer_));
        if (v2Length == 0) {
            state_ = State::Failed;
            lastError_ = Flash;
            sendResponse(request.command, Flash, request.offset);
            return;
        }
        writeWire(wireBuffer_, v2Length);
        return;
    }
    memset(&responseFrame_, 0, sizeof(responseFrame_));
    const bool completedResponse = state_ == State::Idle &&
        transactionId_ == 0 && snapshot_.completed.present &&
        replyTransactionId_ != 0 &&
        snapshot_.completed.identity.transaction_id == replyTransactionId_;
    responseFrame_.target = Target::Right;
    responseFrame_.command = Command::StatusResponse;
    responseFrame_.flags = kFlagResponse;
    responseFrame_.transaction_id = completedResponse
        ? snapshot_.completed.identity.transaction_id
        : transactionId_ != 0 ? transactionId_ : replyTransactionId_;
    // STATUS uses the same resume contract as ACK/NACK: the header offset
    // and StatusPayload.next_offset must name the identical durable byte.
    // Leaving the zero-initialized header here made a fully Prepared image
    // fail the host's ambiguity check even though the payload was correct.
    responseFrame_.offset = bytesWritten_;
    responseFrame_.sequence = replySequence_ == 0 ? 1 : replySequence_;
    StatusPayload status;
    status.state = completedResponse
        ? (snapshot_.completed.outcome ==
               makcm::ota::snapshot::Outcome::Confirmed
             ? makcm::ota::State::Confirmed
             : makcm::ota::State::Idle)
        : toCommonState(state_, maintenanceActive_);
    status.last_command = Command::Status;
    status.reason = completedResponse
        ? NackReason::None
        : toNack(static_cast<Error>(lastError_));
    status.next_offset = bytesWritten_;
    status.target_generation = completedResponse
        ? snapshot_.completed.identity.generation
        : targetGeneration_;
    status.active_generation = kCurrentGeneration;
    status.status_flags = statusFlags(this);
    encodeStatusPayload(status, responseFrame_.payload);
    responseFrame_.length = kStatusPayloadSize;
    const size_t wireLength = encodeFrame(responseFrame_, wireBuffer_,
                                          sizeof(wireBuffer_));
    if (wireLength != 0) writeWire(wireBuffer_, wireLength);
}

bool OtaReceiver::persistState() {
    // V2 snapshot is the sole writable lifecycle record.  Keep this shim for
    // old call sites during the migration window, but never emit a partial
    // multi-key V1 record that could outrank or contradict snap_a/snap_b.
    return snapshotReady_;
}

uint32_t OtaReceiver::completedCrc(const CompletedRecord &record) {
    return makcm::ota::crc32(
        reinterpret_cast<const uint8_t *>(&record),
        offsetof(CompletedRecord, crc));
}

bool OtaReceiver::persistCompletedState() {
    // Completed facts are committed atomically by finalizeCommandResponse().
    return snapshotReady_;
}

bool OtaReceiver::loadCompletedState() {
    completed_ = {};
    completedValid_ = false;
    if (completedPrefs_ == nullptr) return false;
    const bool exists = completedPrefs_->isKey(kCompletedKey);
    const size_t length = completedPrefs_->getBytesLength(kCompletedKey);
    if (!exists) return true;
    if (length == 0) return false;
    if (length != sizeof(CompletedRecord) ||
        completedPrefs_->getBytes(kCompletedKey, &completed_,
                                  sizeof(completed_)) != sizeof(completed_))
        return false;
    completedValid_ = completed_.magic == kCompletedMagic &&
        completed_.version == kCompletedVersion &&
        completed_.transactionId != 0 && completed_.generation != 0 &&
        completed_.crc == completedCrc(completed_);
    if (!completedValid_) return false;

    // Only an exact current image/slot/VALID match can assert local boot
    // validation for the completed transaction.
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t imageState = ESP_OTA_IMG_UNDEFINED;
    localBootValidated_ = completed_.generation == kCurrentGeneration &&
        running != nullptr &&
        completed_.slot == static_cast<uint8_t>(running->subtype) &&
        esp_ota_get_state_partition(running, &imageState) == ESP_OK &&
        imageState == ESP_OTA_IMG_VALID;
    return true;
}

bool OtaReceiver::completedMatches(uint32_t transactionId) const {
    return completedValid_ && transactionId != 0 &&
           completed_.transactionId == transactionId;
}

void OtaReceiver::loadPersistentState() {
    if (prefs_ == nullptr) return;
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    esp_ota_img_states_t imageState = ESP_OTA_IMG_UNDEFINED;
    lastStateQueryError_ = running == nullptr
        ? ESP_ERR_NOT_FOUND
        : esp_ota_get_state_partition(running, &imageState);
    lastImageState_ = imageState;
    if (snapshot_.active.present || snapshot_.completed.present) {
        maintenanceActive_ = snapshot_.active.present;
        if (lastStateQueryError_ != ESP_OK ||
            !restoreFromSnapshotV2(running, boot, imageState)) {
            state_ = State::Failed;
            lastError_ = Flash;
        }
        return;
    }

    // Legacy records are intentionally read-only.  They do not contain the
    // signed manifest/previous-slot identity or replay ledger required by V2;
    // treating them as an empty transaction would silently lose evidence.
    const bool legacyActive = prefs_->isKey("state") ||
                              prefs_->isKey("txid") ||
                              prefs_->isKey("generation");
    const bool legacyCompleted = completedPrefs_ != nullptr &&
        completedPrefs_->isKey(kCompletedKey);
    if (legacyActive || legacyCompleted) {
        state_ = State::Failed;
        lastError_ = Flash;
        return;
    }
    state_ = State::Idle;
    transactionId_ = 0;
    targetGeneration_ = 0;
    targetSubtype_ = 0xFF;
    targetPartition_ = nullptr;
    localBootValidated_ = false;
}

bool OtaReceiver::readSnapshotCopy(
    const char *key, uint8_t *wire,
    makcm::ota::snapshot::Snapshot &decoded, bool &present, bool &valid) {
    present = false;
    valid = false;
    if (prefs_ == nullptr || key == nullptr || wire == nullptr) {
        snapshotCopyError_ = true;
        return false;
    }
    const bool exists = prefs_->isKey(key);
    const size_t length = prefs_->getBytesLength(key);
    if (!exists) return true;
    present = true;
    if (length == 0) {
        snapshotCopyError_ = true;
        return true;
    }
    if (length != makcm::ota::snapshot::kSnapshotWireSize) {
        snapshotCopyError_ = true;
        return true;
    }
    if (prefs_->getBytes(key, wire, length) != length) {
        lastNvsError_ = ESP_FAIL;
        snapshotCopyError_ = true;
        return false;
    }
    valid = makcm::ota::snapshot::decodeSnapshot(wire, length, decoded) &&
            decoded.role == makcm::ota::Target::Right;
    if (!valid) snapshotCopyError_ = true;
    return true;
}

makcm::ota::snapshot::Snapshot &OtaReceiver::beginSnapshotMutation() {
    pendingSnapshot_ = snapshot_;
    return pendingSnapshot_;
}

bool OtaReceiver::commitPendingSnapshotV2() {
    using namespace makcm::ota::snapshot;
    if (prefs_ == nullptr || pendingSnapshot_.role !=
            makcm::ota::Target::Right ||
        snapshot_.commit_sequence == UINT32_MAX) {
        snapshotCopyError_ = true;
        return false;
    }
    pendingSnapshot_.commit_sequence = snapshot_.commit_sequence + 1;
    if (!encodeSnapshot(pendingSnapshot_, snapshotWriteWire_,
                        sizeof(snapshotWriteWire_))) {
        snapshotCopyError_ = true;
        return false;
    }
    const int8_t nextCopy = selectedSnapshotCopy_ == 0 ? 1 : 0;
    const char *key = nextCopy == 0 ? kSnapshotAKey : kSnapshotBKey;
    if (prefs_->putBytes(key, snapshotWriteWire_,
                         sizeof(snapshotWriteWire_)) !=
        sizeof(snapshotWriteWire_)) {
        lastNvsError_ = ESP_FAIL;
        snapshotCopyError_ = true;
        return false;
    }
    const size_t length = prefs_->getBytesLength(key);
    if (length != sizeof(snapshotReadbackWire_) ||
        prefs_->getBytes(key, snapshotReadbackWire_, length) != length ||
        memcmp(snapshotWriteWire_, snapshotReadbackWire_,
               sizeof(snapshotWriteWire_)) != 0) {
        lastNvsError_ = ESP_FAIL;
        snapshotCopyError_ = true;
        return false;
    }
    Snapshot &verified = nextCopy == 0 ? snapshotCandidateA_
                                       : snapshotCandidateB_;
    if (!decodeSnapshot(snapshotReadbackWire_, length, verified) ||
        verified.commit_sequence != pendingSnapshot_.commit_sequence) {
        lastNvsError_ = ESP_FAIL;
        snapshotCopyError_ = true;
        return false;
    }
    snapshot_ = pendingSnapshot_;
    selectedSnapshotCopy_ = nextCopy;
    snapshotReady_ = true;
    lastNvsError_ = ESP_OK;
    snapshotCopyError_ = false;
    return true;
}

bool OtaReceiver::loadSnapshotV2() {
    using namespace makcm::ota::snapshot;
    snapshotCopyError_ = false;
    bool presentA = false;
    bool presentB = false;
    bool validA = false;
    bool validB = false;
    if (!readSnapshotCopy(kSnapshotAKey, snapshotWireA_, snapshotCandidateA_,
                          presentA, validA) ||
        !readSnapshotCopy(kSnapshotBKey, snapshotWireB_, snapshotCandidateB_,
                          presentB, validB)) return false;
    if ((presentA && !validA) && (presentB && !validB)) {
        snapshotCopyError_ = true;
        return false;
    }
    if (!validA && !validB) {
        if (presentA || presentB) {
            snapshotCopyError_ = true;
            return false;
        }
        memset(&snapshot_, 0, sizeof(snapshot_));
        snapshot_.role = makcm::ota::Target::Right;
        selectedSnapshotCopy_ = -1;
        snapshotReady_ = false;
        // commitPendingSnapshotV2() validates pendingSnapshot_, not snapshot_.
        // A default-constructed Snapshot carries role Host, so seeding the
        // pending copy here is what makes the very first commit on an empty
        // NVS legal.  Without it every erased board fails closed into
        // Failed/FlashWriteFailed on each boot.
        pendingSnapshot_ = snapshot_;
        return commitPendingSnapshotV2();
    }
    if (validA && validB &&
        snapshotCandidateA_.commit_sequence ==
        snapshotCandidateB_.commit_sequence) {
        if (memcmp(snapshotWireA_, snapshotWireB_, sizeof(snapshotWireA_)) != 0) {
            snapshotCopyError_ = true;
            return false;
        }
        snapshot_ = snapshotCandidateA_;
        selectedSnapshotCopy_ = 0;
    } else if (validA &&
               (!validB || snapshotCandidateA_.commit_sequence >
                                snapshotCandidateB_.commit_sequence)) {
        snapshot_ = snapshotCandidateA_;
        selectedSnapshotCopy_ = 0;
    } else {
        snapshot_ = snapshotCandidateB_;
        selectedSnapshotCopy_ = 1;
    }
    snapshotReady_ = true;
    return true;
}

const esp_partition_t *OtaReceiver::partitionForSubtype(uint8_t subtype) const {
    if (subtype == 0xFF) return nullptr;
    return esp_partition_find_first(
        ESP_PARTITION_TYPE_APP,
        static_cast<esp_partition_subtype_t>(subtype), nullptr);
}

bool OtaReceiver::partitionMatchesImage(
    const esp_partition_t *partition,
    const makcm::ota::snapshot::Identity &identity) const {
    if (partition == nullptr || identity.image_size == 0 ||
        identity.image_size > partition->size ||
        static_cast<uint8_t>(partition->subtype) != identity.target_slot)
        return false;
    // identity.image_sha256 covers the full transferred image including the
    // esptool-appended digest trailer.  esp_partition_get_sha256() returns the
    // app-image digest, which excludes that trailer, so the two never compare
    // equal for a hash-appended image; hash the raw partition bytes instead.
    uint8_t digest[32] = {};
    mbedtls_sha256_context hash;
    mbedtls_sha256_init(&hash);
    bool ok = mbedtls_sha256_starts_ret(&hash, 0) == 0;
    uint8_t buffer[1024];
    for (uint32_t offset = 0; ok && offset < identity.image_size;) {
        const uint32_t chunk =
            identity.image_size - offset < sizeof(buffer)
                ? identity.image_size - offset
                : static_cast<uint32_t>(sizeof(buffer));
        ok = esp_partition_read(partition, offset, buffer, chunk) == ESP_OK &&
             mbedtls_sha256_update_ret(&hash, buffer, chunk) == 0;
        offset += chunk;
    }
    ok = ok && mbedtls_sha256_finish_ret(&hash, digest) == 0;
    mbedtls_sha256_free(&hash);
    return ok && memcmp(digest, identity.image_sha256, sizeof(digest)) == 0;
}

bool OtaReceiver::partitionMatchesPreviousImage(
    const esp_partition_t *partition,
    const makcm::ota::snapshot::Identity &identity) const {
    if (partition == nullptr ||
        static_cast<uint8_t>(partition->subtype) != identity.previous_slot)
        return false;
    uint8_t digest[32] = {};
    return esp_partition_get_sha256(partition, digest) == ESP_OK &&
           memcmp(digest, identity.previous_image_sha256,
                  sizeof(digest)) == 0;
}

bool OtaReceiver::identityDigestMatches(
    const makcm::ota::snapshot::Identity &identity) const {
    using namespace makcm::ota::snapshot;
    Identity canonicalIdentity = identity;
    memset(canonicalIdentity.identity_digest, 0,
           sizeof(canonicalIdentity.identity_digest));
    uint8_t canonical[kIdentitySize] = {};
    encodeIdentity(canonicalIdentity, canonical);
    uint8_t digest[32] = {};
    return mbedtls_sha256_ret(canonical, sizeof(canonical), digest, 0) == 0 &&
           memcmp(digest, identity.identity_digest, sizeof(digest)) == 0;
}

bool OtaReceiver::restoreFromSnapshotV2(const esp_partition_t *running,
                                        const esp_partition_t *boot,
                                        esp_ota_img_states_t imageState) {
    using namespace makcm::ota;
    using namespace makcm::ota::snapshot;
    transactionId_ = 0;
    targetGeneration_ = 0;
    imageSize_ = 0;
    targetSubtype_ = 0xFF;
    targetPartition_ = nullptr;
    manifest_ = {};
    manifestLength_ = 0;
    manifestWire_[0] = 0;
    manifest_ = {};
    manifestLength_ = 0;
    localBootValidated_ = false;
    bytesWritten_ = 0;
    hashOpen_ = false;
    if (!snapshot_.active.present) {
        if (!snapshot_.completed.present) {
            state_ = State::Idle;
            maintenanceActive_ = false;
            return true;
        }
        const auto &completed = snapshot_.completed;
        if (!identityDigestMatches(completed.identity))
            return false;
        if (completed.outcome == Outcome::Confirmed) {
            if (completed.identity.generation != kCurrentGeneration ||
                running == nullptr || boot == nullptr || running != boot ||
                imageState != ESP_OTA_IMG_VALID || !completed.local_validated ||
                !completed.pair_committed ||
                !partitionMatchesImage(running, completed.identity)) return false;
            localBootValidated_ = true;
        } else if (completed.outcome == Outcome::Aborted ||
                   completed.outcome == Outcome::RolledBack) {
            if (completed.identity.previous_generation != kCurrentGeneration ||
                running == nullptr || boot == nullptr || running != boot ||
                imageState != ESP_OTA_IMG_VALID ||
                !partitionMatchesPreviousImage(running, completed.identity))
                return false;
        } else {
            return false;
        }
        state_ = State::Idle;
        maintenanceActive_ = false;
        return true;
    }

    const auto &active = snapshot_.active;
    const auto &identity = active.identity;
    if (!identityDigestMatches(identity) || running == nullptr || boot == nullptr)
        return false;
    const esp_partition_t *target = partitionForSubtype(identity.target_slot);
    const esp_partition_t *previous = partitionForSubtype(identity.previous_slot);
    if (target == nullptr || previous == nullptr || target == previous)
        return false;
    transactionId_ = identity.transaction_id;
    targetGeneration_ = identity.generation;
    minPeerGeneration_ = identity.min_peer_generation;
    maxPeerGeneration_ = identity.max_peer_generation;
    imageSize_ = identity.image_size;
    targetSubtype_ = identity.target_slot;
    targetPartition_ = target;
    peerGeneration_ = identity.previous_generation;
    memcpy(imageSha256_, identity.image_sha256, sizeof(imageSha256_));
    manifest_.role = identity.role;
    manifest_.protocol_version = static_cast<uint16_t>(identity.protocol_epoch);
    manifest_.generation = identity.generation;
    manifest_.min_peer_generation = identity.min_peer_generation;
    manifest_.max_peer_generation = identity.max_peer_generation;
    manifest_.partition_schema = identity.partition_schema;
    manifest_.image_size = identity.image_size;
    memcpy(manifest_.image_sha256, identity.image_sha256,
           sizeof(manifest_.image_sha256));
    memcpy(manifest_.hardware_revision, identity.hardware_revision,
           sizeof(manifest_.hardware_revision));
    memcpy(manifest_.project_name, identity.project_name,
           sizeof(manifest_.project_name));
    const uint8_t runningSlot = static_cast<uint8_t>(running->subtype);
    const uint8_t bootSlot = static_cast<uint8_t>(boot->subtype);
    const bool runningTarget = runningSlot == identity.target_slot;
    const bool runningPrevious = runningSlot == identity.previous_slot;
    const bool bootTarget = bootSlot == identity.target_slot;
    const bool bootPrevious = bootSlot == identity.previous_slot;
    if (active.phase == Phase::Receiving) {
        if (!runningPrevious || !bootPrevious ||
            identity.generation <= kCurrentGeneration) return false;
        // Volatile esp_ota handle/stream hash cannot be reconstructed.  The
        // exact signed MANIFEST must be replayed from offset zero.
        state_ = State::Failed;
        maintenanceActive_ = true;
        lastError_ = Timeout;
        return true;
    }
    if (active.phase == Phase::Prepared) {
        if (!runningPrevious || !bootPrevious ||
            identity.generation <= kCurrentGeneration ||
            !partitionMatchesImage(target, identity)) return false;
        bytesWritten_ = imageSize_;
        state_ = State::Prepared;
        maintenanceActive_ = true;
        return true;
    }
    if (active.phase == Phase::ActivateIntent ||
        active.phase == Phase::BootSelected) {
        if (!partitionMatchesImage(target, identity))
            return false;
        if (runningPrevious) {
            if (identity.generation <= kCurrentGeneration) return false;
            if (bootPrevious) {
                bytesWritten_ = imageSize_;
                state_ = State::Prepared;
                maintenanceActive_ = true;
                return true;
            }
            if (bootTarget) {
                state_ = State::Activated;
                maintenanceActive_ = true;
                return true;
            }
            return false;
        }
        if (runningTarget && bootTarget &&
            identity.generation == kCurrentGeneration &&
            partitionMatchesImage(running, identity)) {
            if (imageState == ESP_OTA_IMG_PENDING_VERIFY) {
                state_ = State::AwaitConfirm;
                maintenanceActive_ = true;
                return true;
            }
            if (imageState == ESP_OTA_IMG_VALID) {
                localBootValidated_ = true;
                state_ = State::AwaitConfirm;
                maintenanceActive_ = true;
                if (!active.local_validated && !persistLocalValidationFact())
                    return false;
                return true;
            }
        }
        return false;
    }
    if (active.phase == Phase::PendingVerify ||
        active.phase == Phase::LocalValidated ||
        active.phase == Phase::PairPending) {
        if (!runningTarget || !bootTarget ||
            identity.generation != kCurrentGeneration ||
            !partitionMatchesImage(running, identity)) return false;
        state_ = State::AwaitConfirm;
        maintenanceActive_ = true;
        if (imageState == ESP_OTA_IMG_PENDING_VERIFY) {
            if (active.local_validated) return false;
            return true;
        }
        if (imageState != ESP_OTA_IMG_VALID) return false;
        localBootValidated_ = true;
        if (!active.local_validated && !persistLocalValidationFact()) return false;
        return true;
    }
    if (active.phase == Phase::RollbackIntent ||
        active.phase == Phase::RolledBack) {
        if (!runningPrevious || !bootPrevious ||
            imageState != ESP_OTA_IMG_VALID ||
            !partitionMatchesPreviousImage(running, identity)) return false;
        if (active.phase == Phase::RollbackIntent) {
            auto &next = beginSnapshotMutation();
            auto &entry = next.active.ledger[ledgerIndex(Command::Abort)];
            if (!entry.valid || entry.command != Command::Abort) return false;
            entry.effect_phase = EffectPhase::ExternalEffectObserved;
            entry.result = NackReason::None;
            entry.resulting_state = makcm::ota::State::Idle;
            next.active.phase = Phase::RolledBack;
            next.active.running_slot = runningSlot;
            next.active.boot_slot = bootSlot;
            next.active.image_state = static_cast<uint8_t>(imageState);
            if (!commitPendingSnapshotV2()) return false;
        }
        state_ = State::Idle;
        maintenanceActive_.store(true, std::memory_order_release);
        lastError_ = Ok;
        return true;
    }
    if (active.phase == Phase::Failed) {
        state_ = State::Failed;
        maintenanceActive_ = true;
        lastError_ = Flash;
        return true;
    }
    return false;
}

bool OtaReceiver::buildActiveIdentity(
    const makcm::ota::Frame &manifestRequest) {
    using namespace makcm::ota;
    using namespace makcm::ota::snapshot;
    if (targetPartition_ == nullptr || manifestRequest.command != Command::Manifest ||
        manifestRequest.transaction_id == 0 || manifestLength_ != kManifestLength)
        return false;
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    if (running == nullptr || boot == nullptr) return false;
    auto &active = pendingSnapshot_.active;
    memset(&active, 0, sizeof(active));
    active.present = true;
    active.phase = Phase::Receiving;
    active.running_slot = static_cast<uint8_t>(running->subtype);
    active.boot_slot = static_cast<uint8_t>(boot->subtype);
    active.image_state = static_cast<uint8_t>(lastImageState_);
    active.reset_reason = static_cast<uint32_t>(resetReason_);
    active.last_ota_operation = static_cast<uint8_t>(Command::Manifest);
    active.identity.protocol_epoch = kProtocolEpoch;
    active.identity.role = Target::Right;
    active.identity.target_slot = static_cast<uint8_t>(targetPartition_->subtype);
    active.identity.previous_slot = static_cast<uint8_t>(running->subtype);
    active.identity.transaction_id = manifestRequest.transaction_id;
    active.identity.generation = manifest_.generation;
    active.identity.previous_generation = kCurrentGeneration;
    active.identity.partition_schema = manifest_.partition_schema;
    active.identity.image_size = manifest_.image_size;
    active.identity.min_peer_generation = manifest_.min_peer_generation;
    active.identity.max_peer_generation = manifest_.max_peer_generation;
    memcpy(active.identity.hardware_revision, manifest_.hardware_revision,
           sizeof(active.identity.hardware_revision));
    memcpy(active.identity.project_name, manifest_.project_name,
           sizeof(active.identity.project_name));
    memcpy(active.identity.image_sha256, manifest_.image_sha256,
           sizeof(active.identity.image_sha256));
    if (esp_partition_get_sha256(running, active.identity.previous_image_sha256) !=
        ESP_OK ||
        mbedtls_sha256_ret(manifestWire_, manifestLength_,
                           active.identity.manifest_sha256, 0) != 0)
        return false;
    Identity canonical = active.identity;
    memset(canonical.identity_digest, 0, sizeof(canonical.identity_digest));
    uint8_t wire[kIdentitySize] = {};
    encodeIdentity(canonical, wire);
    return mbedtls_sha256_ret(wire, sizeof(wire),
                              active.identity.identity_digest, 0) == 0;
}

bool OtaReceiver::hashRequest(const makcm::ota::Frame &request,
                              uint8_t out[32]) const {
    using namespace makcm::ota;
    if (out == nullptr || request.length > kMaxChunkSize) return false;
    uint8_t header[20] = {};
    header[0] = static_cast<uint8_t>(request.target);
    header[1] = static_cast<uint8_t>(request.command);
    header[2] = static_cast<uint8_t>(request.flags & ~kFlagRetransmit);
    writeLe32(header + 4, request.transaction_id);
    writeLe32(header + 8, request.offset);
    writeLe32(header + 12, request.sequence);
    writeLe16(header + 16, request.length);
    mbedtls_sha256_context hash;
    mbedtls_sha256_init(&hash);
    const bool ok = mbedtls_sha256_starts_ret(&hash, 0) == 0 &&
        mbedtls_sha256_update_ret(&hash, header, sizeof(header)) == 0 &&
        (request.length == 0 ||
         mbedtls_sha256_update_ret(&hash, request.payload,
                                   request.length) == 0) &&
        mbedtls_sha256_finish_ret(&hash, out) == 0;
    mbedtls_sha256_free(&hash);
    return ok;
}

bool OtaReceiver::setLedgerIntent(
    makcm::ota::snapshot::ActiveTransaction &active,
    const makcm::ota::Frame &request, makcm::ota::State acceptedFrom,
    makcm::ota::snapshot::Phase phase) {
    using namespace makcm::ota;
    using namespace makcm::ota::snapshot;
    const size_t index = ledgerIndex(request.command);
    if (!active.present || index >= kLedgerCount || request.sequence == 0 ||
        request.sequence == UINT32_MAX) return false;
    auto &entry = active.ledger[index];
    memset(&entry, 0, sizeof(entry));
    entry.valid = true;
    entry.command = request.command;
    entry.effect_phase = EffectPhase::IntentDurable;
    entry.accepted_from_state = acceptedFrom;
    entry.sequence = request.sequence;
    if (!hashRequest(request, entry.request_sha256)) return false;
    entry.resulting_state = acceptedFrom;
    active.phase = phase;
    active.last_request_sequence = request.sequence;
    active.last_command = request.command;
    active.last_nack = NackReason::None;
    active.reset_reason = static_cast<uint32_t>(resetReason_);
    active.last_ota_error = static_cast<int32_t>(lastOtaError_);
    active.last_state_error = static_cast<int32_t>(lastStateQueryError_);
    active.last_ota_operation = static_cast<uint8_t>(request.command);
    return true;
}

bool OtaReceiver::persistCommandIntent(
    const makcm::ota::Frame &request, makcm::ota::State acceptedFrom,
    makcm::ota::snapshot::Phase phase) {
    auto &next = beginSnapshotMutation();
    if (!next.active.present ||
        next.active.identity.transaction_id != request.transaction_id ||
        !setLedgerIntent(next.active, request, acceptedFrom, phase)) return false;
    return commitPendingSnapshotV2();
}

bool OtaReceiver::persistCommandFact(
    const makcm::ota::Frame &request,
    makcm::ota::snapshot::EffectPhase effect,
    makcm::ota::snapshot::Phase phase, makcm::ota::NackReason result,
    makcm::ota::State resultingState) {
    using namespace makcm::ota;
    using namespace makcm::ota::snapshot;
    const size_t index = ledgerIndex(request.command);
    if (index >= kLedgerCount) return false;
    uint8_t requestHash[32] = {};
    if (!hashRequest(request, requestHash)) {
        state_ = State::Failed;
        lastError_ = Flash;
        return false;
    }
    auto &next = beginSnapshotMutation();
    if (!next.active.present ||
        next.active.identity.transaction_id != request.transaction_id)
        return false;
    auto &entry = next.active.ledger[index];
    if (!entry.valid || entry.command != request.command ||
        entry.sequence != request.sequence ||
        memcmp(entry.request_sha256, requestHash, sizeof(requestHash)) != 0)
        return false;
    entry.effect_phase = effect;
    entry.result = result;
    entry.resulting_state = resultingState;
    next.active.phase = phase;
    next.active.next_offset = bytesWritten_;
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    next.active.running_slot = running == nullptr ? 0xFF :
        static_cast<uint8_t>(running->subtype);
    next.active.boot_slot = boot == nullptr ? 0xFF :
        static_cast<uint8_t>(boot->subtype);
    next.active.image_state = static_cast<uint8_t>(lastImageState_);
    // active.local_validated describes THIS transaction's installed image, not
    // the board.  validSnapshot() requires it to equal `phase is LocalValidated
    // or PairPending`, so writing the board-wide localBootValidated_ made every
    // fact persisted in any other phase fail its own encoder -- and on a
    // healthy board that flag is true.  The rejection happens before any NVS
    // call and reaches the host as the opaque FlashWriteFailed.
    // persistLocalValidationFact() remains the one place that sets the flag,
    // together with the phase that justifies it.
    next.active.local_validated =
        phase == makcm::ota::snapshot::Phase::LocalValidated ||
        phase == makcm::ota::snapshot::Phase::PairPending;
    next.active.last_nack = result;
    next.active.last_ota_error = static_cast<int32_t>(lastOtaError_);
    next.active.last_state_error = static_cast<int32_t>(lastStateQueryError_);
    const bool committed = commitPendingSnapshotV2();
    if (!committed) {
        state_ = State::Failed;
        lastError_ = Flash;
    }
    return committed;
}

bool OtaReceiver::persistFactOrFail(
    const makcm::ota::Frame &request,
    makcm::ota::snapshot::EffectPhase effect,
    makcm::ota::snapshot::Phase phase, makcm::ota::NackReason result,
    makcm::ota::State resultingState, Error &error) {
    if (persistCommandFact(request, effect, phase, result, resultingState))
        return true;
    state_ = State::Failed;
    lastError_ = Flash;
    error = Flash;
    return false;
}

void OtaReceiver::fillReplayResponse(
    const makcm::ota::Frame &request,
    const makcm::ota::snapshot::LedgerEntry &entry,
    makcm::ota::Frame &response) const {
    using namespace makcm::ota;
    memset(&response, 0, sizeof(response));
    response.target = Target::Right;
    response.command = entry.result == NackReason::None ? Command::Ack
                                                         : Command::Nack;
    response.flags = kFlagResponse;
    response.transaction_id = request.transaction_id;
    response.sequence = request.sequence;
    response.length = entry.response_length;
    if (entry.response_length != 0) {
        memcpy(response.payload, entry.response, entry.response_length);
        response.offset = entry.response_length >= kStatusPayloadSize
            ? readLe32(entry.response + 8) : 0;
    }
}

void OtaReceiver::fillTombstoneResponse(
    const makcm::ota::Frame &request,
    const makcm::ota::snapshot::CompletedTombstone &completed,
    makcm::ota::Frame &response) const {
    using namespace makcm::ota;
    memset(&response, 0, sizeof(response));
    response.target = Target::Right;
    response.command = completed.result == NackReason::None ? Command::Ack
                                                              : Command::Nack;
    response.flags = kFlagResponse;
    response.transaction_id = request.transaction_id;
    response.sequence = request.sequence;
    response.length = completed.response_length;
    if (completed.response_length != 0) {
        memcpy(response.payload, completed.response, completed.response_length);
        response.offset = completed.response_length >= kStatusPayloadSize
            ? readLe32(completed.response + 8) : 0;
    }
}

bool OtaReceiver::finalizeCommandResponse(
    const makcm::ota::Frame &request, const makcm::ota::Frame &response,
    makcm::ota::NackReason result, bool accepted) {
    using namespace makcm::ota;
    using namespace makcm::ota::snapshot;
    const size_t index = ledgerIndex(request.command);
    if (!snapshotReady_ || index >= kLedgerCount || !snapshot_.active.present ||
        snapshot_.active.identity.transaction_id != request.transaction_id)
        return true;
    uint8_t requestHash[32] = {};
    if (!hashRequest(request, requestHash)) {
        state_ = State::Failed;
        lastError_ = Flash;
        return false;
    }
    auto &next = beginSnapshotMutation();
    auto &entry = next.active.ledger[index];
    if (!entry.valid || entry.command != request.command ||
        entry.sequence != request.sequence ||
        memcmp(entry.request_sha256, requestHash, sizeof(requestHash)) != 0)
        return true;
    entry.effect_phase = EffectPhase::Done;
    entry.result = result;
    entry.resulting_state = state_ == OtaReceiver::State::Confirmed
        ? makcm::ota::State::Confirmed
        : state_ == OtaReceiver::State::Idle
        ? makcm::ota::State::Idle
        : entry.resulting_state;
    entry.response_length = response.length <= kStatusPayloadSize
        ? static_cast<uint8_t>(response.length) : 0;
    entry.snapshot_commit_sequence = snapshot_.commit_sequence + 1;
    memset(entry.response, 0, sizeof(entry.response));
    if (entry.response_length != 0)
        memcpy(entry.response, response.payload, entry.response_length);
    if (accepted && (request.command == Command::Confirm ||
                     request.command == Command::Abort)) {
        auto &completed = next.completed;
        memset(&completed, 0, sizeof(completed));
        completed.present = true;
        completed.outcome = request.command == Command::Confirm
            ? Outcome::Confirmed : Outcome::Aborted;
        completed.terminal_command = request.command;
        completed.final_state = request.command == Command::Confirm
            ? makcm::ota::State::Confirmed : makcm::ota::State::Idle;
        completed.terminal_sequence = request.sequence;
        completed.result = result;
        completed.response_length = entry.response_length;
        const esp_partition_t *running = esp_ota_get_running_partition();
        const esp_partition_t *boot = esp_ota_get_boot_partition();
        completed.running_slot = running == nullptr ? 0xFF :
            static_cast<uint8_t>(running->subtype);
        completed.boot_slot = boot == nullptr ? 0xFF :
            static_cast<uint8_t>(boot->subtype);
        completed.image_state = static_cast<uint8_t>(lastImageState_);
        completed.local_validated = localBootValidated_;
        completed.pair_committed = request.command == Command::Confirm;
        completed.identity = next.active.identity;
        memcpy(completed.terminal_request_sha256, requestHash,
               sizeof(requestHash));
        if (entry.response_length != 0)
            memcpy(completed.response, entry.response, entry.response_length);
        memset(&next.active, 0, sizeof(next.active));
        state_ = request.command == Command::Confirm ? State::Confirmed
                                                       : State::Idle;
    }
    const bool committed = commitPendingSnapshotV2();
    if (!committed) {
        state_ = State::Failed;
        lastError_ = Flash;
    }
    return committed;
}

bool OtaReceiver::persistLocalValidationFact() {
    using namespace makcm::ota::snapshot;
    if (!snapshot_.active.present || transactionId_ == 0 ||
        snapshot_.active.identity.transaction_id != transactionId_ ||
        targetPartition_ == nullptr ||
        static_cast<uint8_t>(targetPartition_->subtype) !=
            snapshot_.active.identity.target_slot ||
        !partitionMatchesImage(targetPartition_, snapshot_.active.identity))
        return false;
    auto &next = beginSnapshotMutation();
    next.active.phase = Phase::LocalValidated;
    next.active.local_validated = true;
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    next.active.running_slot = running == nullptr ? 0xFF :
        static_cast<uint8_t>(running->subtype);
    next.active.boot_slot = boot == nullptr ? 0xFF :
        static_cast<uint8_t>(boot->subtype);
    next.active.image_state = static_cast<uint8_t>(ESP_OTA_IMG_VALID);
    next.active.reset_reason = static_cast<uint32_t>(resetReason_);
    next.active.last_ota_error = static_cast<int32_t>(markValidError_);
    next.active.last_state_error = static_cast<int32_t>(lastStateQueryError_);
    return commitPendingSnapshotV2();
}

OtaReceiver::ReplayDecision OtaReceiver::replayCommand(
    const makcm::ota::Frame &request, makcm::ota::Frame &response) const {
    using namespace makcm::ota;
    using namespace makcm::ota::snapshot;
    const size_t index = ledgerIndex(request.command);
    if (!snapshotReady_ || index >= kLedgerCount) return ReplayDecision::NewCommand;
    uint8_t requestHash[32] = {};
    if (!hashRequest(request, requestHash)) return ReplayDecision::Conflict;
    if (snapshot_.completed.present &&
        request.transaction_id == snapshot_.completed.identity.transaction_id) {
        const auto &completed = snapshot_.completed;
        if (request.command == completed.terminal_command &&
            request.sequence == completed.terminal_sequence &&
            memcmp(requestHash, completed.terminal_request_sha256,
                   sizeof(requestHash)) == 0 && completed.response_length != 0) {
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
            memcmp(requestHash, entry.request_sha256, sizeof(requestHash)) != 0)
            return ReplayDecision::Conflict;
        if (entry.command == Command::Manifest && otaHandle_ == 0 &&
            (state_ == OtaReceiver::State::Failed ||
             state_ == OtaReceiver::State::Idle))
            return ReplayDecision::ResumeIntent;
        if (entry.effect_phase == EffectPhase::IntentDurable)
            return ReplayDecision::ResumeIntent;
        if (entry.effect_phase == EffectPhase::ExternalEffectObserved)
            return ReplayDecision::ResumeObserved;
        if (entry.effect_phase != EffectPhase::Done || entry.response_length == 0)
            return ReplayDecision::Conflict;
        fillReplayResponse(request, entry, response);
        return ReplayDecision::Replayed;
    }
    for (const auto &other : snapshot_.active.ledger) {
        if (other.valid && other.sequence == request.sequence)
            return ReplayDecision::Conflict;
    }
    if (request.sequence == 0 || request.sequence == UINT32_MAX ||
        request.sequence <= snapshot_.active.last_request_sequence)
        return ReplayDecision::Conflict;
    return ReplayDecision::NewCommand;
}

bool OtaReceiver::resumeIncompleteCommand(
    const makcm::ota::Frame &request, ReplayDecision decision,
    makcm::ota::Frame &response, Error &error) {
    using namespace makcm::ota;
    using namespace makcm::ota::snapshot;
    const size_t index = ledgerIndex(request.command);
    if (!snapshot_.active.present || index >= kLedgerCount ||
        snapshot_.active.identity.transaction_id != request.transaction_id) {
        error = BadTransaction;
        return false;
    }
    auto completeObserved = [&]() -> bool {
        fillResponse(request, Command::Ack, NackReason::None, response);
        if (!finalizeCommandResponse(request, response, NackReason::None, true)) {
            error = Flash;
            return false;
        }
        error = Ok;
        return true;
    };
    if (decision == ReplayDecision::ResumeObserved) {
        // The IDF handle cannot survive a reboot.  Manifest must be replayed;
        // other observed effects are reconciled from partition facts.
        if (request.command != Command::Manifest || otaHandle_ != 0)
            return completeObserved();
        state_ = State::Failed;
    }
    bool ok = false;
    switch (request.command) {
    case Command::Manifest: {
        if (state_ != State::Idle && state_ != State::Failed) {
            error = BadState;
            break;
        }
        ok = validateManifest(request.payload, request.length, error);
        if (ok) {
            transactionId_ = request.transaction_id;
            bytesWritten_ = 0;
            lastError_ = Ok;
            lastOtaError_ =
                esp_ota_begin(targetPartition_, imageSize_, &otaHandle_);
            if (lastOtaError_ != ESP_OK) {
                otaHandle_ = 0;
                error = Flash;
                state_ = State::Failed;
                break;
            }
            state_ = State::Receiving;
            hashOpen_ = mbedtls_sha256_starts_ret(&imageHash_, 0) == 0;
            if (!hashOpen_) {
                error = HashMismatch;
                break;
            }
            if (!persistFactOrFail(request, EffectPhase::ExternalEffectObserved,
                                   Phase::Receiving, NackReason::None,
                                   makcm::ota::State::Receiving, error)) {
                ok = false;
            } else {
                ok = true;
            }
        }
        break;
    }
    case Command::Prepare:
        if (state_ == State::Prepared && targetPartition_ != nullptr &&
            partitionMatchesImage(targetPartition_, snapshot_.active.identity)) {
            if (!persistFactOrFail(request,
                                   EffectPhase::ExternalEffectObserved,
                                   Phase::Prepared, NackReason::None,
                                   makcm::ota::State::Prepared, error)) {
                break;
            }
            return completeObserved();
        }
        ok = prepareImage(request, error);
        break;
    case Command::Activate:
        if (state_ == State::Prepared) ok = handleActivate(request, error);
        else if (state_ == State::Activated) {
            const esp_partition_t *boot = esp_ota_get_boot_partition();
            if (boot == targetPartition_) {
                if (!persistFactOrFail(request,
                                       EffectPhase::ExternalEffectObserved,
                                       Phase::BootSelected, NackReason::None,
                                       makcm::ota::State::ActivationPending,
                                       error)) {
                    break;
                }
                ok = true;
            } else {
                error = BadPartition;
            }
        } else {
            error = BadState;
        }
        break;
    case Command::Confirm:
        ok = handleConfirm(request, error);
        break;
    case Command::Abort:
        error = RollbackUnavailable;
        break;
    default:
        error = BadState;
        break;
    }
    fillResponse(request, ok ? Command::Ack : Command::Nack, toNack(error),
                 response);
    if (!finalizeCommandResponse(request, response, toNack(error), ok)) {
        error = Flash;
        fillResponse(request, Command::Nack, NackReason::FlashWriteFailed,
                     response);
        return false;
    }
    return ok;
}

bool OtaReceiver::validateManifest(const uint8_t *payload, uint16_t length,
                                   Error &error) {
    using namespace makcm::ota;
    if (payload == nullptr || length != kManifestLength ||
        !decodeManifest(payload, length, manifest_)) {
        error = BadManifest;
        return false;
    }
    if (manifest_.role != Target::Right ||
        manifest_.image_kind != kImageKindApplication) {
        error = BadRole;
        return false;
    }
    if (manifest_.protocol_version != makcm::ota::board::kPairProtocolVersion) {
        error = BadVersion;
        return false;
    }
    if (manifest_.partition_schema !=
        makcm::ota::board::kRightPartitionSchemaV1) {
        error = BadPartition;
        return false;
    }
    if (strcmp(manifest_.hardware_revision,
               makcm::ota::board::kHardwareRevisionR1) != 0 ||
        strcmp(manifest_.project_name,
               makcm::ota::board::kRightManifestProjectName) != 0 ||
        manifest_.generation <= kCurrentGeneration ||
        manifest_.min_peer_generation == 0 ||
        manifest_.max_peer_generation == 0 ||
        manifest_.min_peer_generation > manifest_.max_peer_generation ||
        (peerGeneration_ == 0 ||
         !makcm::ota::manifestCompatibleWithPeer(manifest_, peerGeneration_))) {
        error = BadHardware;
        return false;
    }
    const esp_partition_t *running = esp_ota_get_running_partition();
    targetPartition_ = esp_ota_get_next_update_partition(running);
    if (!isOtaApplication(targetPartition_) || targetPartition_ == running) {
        error = BadPartition;
        return false;
    }
    targetSubtype_ = static_cast<uint8_t>(targetPartition_->subtype);
    if (manifest_.image_size == 0 || manifest_.image_size > targetPartition_->size ||
        manifest_.image_size < 32 ||
        manifest_.chunk_size < makcm::ota::kMinChunkSize ||
        manifest_.chunk_size > makcm::ota::kMaxChunkSize) {
        error = BadImage;
        return false;
    }
    imageSize_ = manifest_.image_size;
    targetGeneration_ = manifest_.generation;
    minPeerGeneration_ = manifest_.min_peer_generation;
    maxPeerGeneration_ = manifest_.max_peer_generation;
    memcpy(imageSha256_, manifest_.image_sha256, sizeof(imageSha256_));
    memcpy(manifestWire_, payload, sizeof(manifestWire_));
    manifestLength_ = sizeof(manifestWire_);
    lastSequence_ = 0;
    haveSequence_ = false;
    if (!verifySignature(payload, length)) {
        error = SignatureRequired;
        return false;
    }
    error = Ok;
    return true;
}

bool OtaReceiver::verifySignature(const uint8_t *wire, uint16_t length) const {
    if (wire == nullptr || length != makcm::ota::kManifestWireSize ||
        memcmp(wire + 112, makcm::ota::trust::kSignerKeyId,
               sizeof(makcm::ota::trust::kSignerKeyId)) != 0)
        return false;
    uint8_t digest[32] = {};
    return mbedtls_sha256_ret(wire, makcm::ota::kManifestSignedSize,
                             digest, 0) == 0 &&
           verifyP256(digest, wire + makcm::ota::kManifestSignedSize);
}

// GO-1 diagnostic counters (remove after GO-1).  One totals crumb at Prepared
// discriminates the per-chunk retransmit mechanism: dup>0 means the first
// transmission actually landed and its ACK was lost upstream; ferr/ovf tracking
// dat means the first transmission died on this board's RX path; all-zero
// alongside 10 s/chunk means the first transmission never left the left board.
extern volatile uint32_t serial1RxFifoOvf;
extern volatile uint32_t serial1RxBufferFull;
extern volatile uint32_t serial1RxFrameErr;
static uint32_t g_otaDataFrames = 0;
static uint32_t g_otaDupAccepts = 0;
static uint32_t g_otaDecoderFrameErrors = 0;

bool OtaReceiver::handleData(uint32_t sequence, uint32_t offset,
                             const uint8_t *payload, uint16_t length,
                             Error &error) {
    ++g_otaDataFrames;
    if (state_ != State::Receiving || otaHandle_ == 0 || !hashOpen_ ||
        payload == nullptr ||
        length == 0 || length > makcm::ota::kMaxChunkSize ||
        (length < makcm::ota::kMinChunkSize && offset + length != imageSize_)) {
        error = BadState;
        return false;
    }
    if (offset > imageSize_ || length > imageSize_ - offset) {
        error = BadOffset;
        return false;
    }
    const uint32_t dataCrc = makcm::ota::crc32(payload, length);
    uint8_t dataSha256[32] = {};
    if (mbedtls_sha256_ret(payload, length, dataSha256, 0) != 0) {
        error = HashMismatch;
        return false;
    }
    if (offset == lastDataOffset_ && length == lastDataLength_ &&
        sequence == lastDataSequence_ && dataCrc == lastDataCrc_ &&
        memcmp(dataSha256, lastDataSha256_, sizeof(dataSha256)) == 0 &&
        offset < bytesWritten_) {
        ++g_otaDupAccepts;
        error = Ok;
        return true;
    }
    if (offset < bytesWritten_) {
        if (offset + length > bytesWritten_ ||
            esp_partition_read(targetPartition_, offset, s_compareBuffer, length) != ESP_OK ||
            memcmp(s_compareBuffer, payload, length) != 0) {
            error = BadOffset;
            return false;
        }
        ++g_otaDupAccepts;
        error = Ok;
        return true;
    }
    if (offset != bytesWritten_) {
        error = BadOffset;
        return false;
    }
    if (offset == 0) {
        const size_t appDescriptorOffset = sizeof(esp_image_header_t) +
                                           sizeof(esp_image_segment_header_t);
        if (length < appDescriptorOffset + sizeof(esp_app_desc_t) ||
            payload[0] != ESP_IMAGE_HEADER_MAGIC) {
            error = BadImage;
            return false;
        }
        esp_app_desc_t descriptor = {};
        memcpy(&descriptor, payload + appDescriptorOffset, sizeof(descriptor));
        if (descriptor.magic_word != ESP_APP_DESC_MAGIC_WORD ||
            strcmp(descriptor.project_name,
                   makcm::ota::board::kRightAppDescriptorProjectName) != 0) {
            error = BadImage;
            return false;
        }
    }
    lastOtaError_ = esp_ota_write(otaHandle_, payload, length);
    if (lastOtaError_ != ESP_OK ||
        mbedtls_sha256_update_ret(&imageHash_, payload, length) != 0) {
        error = Flash;
        return false;
    }
    lastDataOffset_ = offset;
    lastDataLength_ = length;
    lastDataCrc_ = dataCrc;
    lastDataSequence_ = sequence;
    memcpy(lastDataSha256_, dataSha256, sizeof(lastDataSha256_));
    bytesWritten_ += length;
    error = Ok;
    return true;
}

bool OtaReceiver::prepareImage(const makcm::ota::Frame &request, Error &error) {
    if (state_ != State::Receiving || otaHandle_ == 0 ||
        targetPartition_ == nullptr || bytesWritten_ != imageSize_) {
        error = BadState;
        return false;
    }
    if (!persistCommandIntent(request, makcm::ota::State::Receiving,
                              makcm::ota::snapshot::Phase::Receiving)) {
        error = Flash;
        return false;
    }
    auto failAndAbort = [&](Error fallback) {
        const esp_err_t abortError = esp_ota_abort(otaHandle_);
        lastOtaError_ = abortError;
        otaHandle_ = 0;
        state_ = State::Failed;
        if (abortError != ESP_OK) {
            // An abort failure is itself a flash fault.  Do not report the
            // earlier image/hash reason as if the download were safely closed.
            lastError_ = Flash;
            error = Flash;
        } else {
            lastError_ = fallback;
            error = fallback;
        }
    };
    uint8_t imageHeader[4] = {};
    if (esp_partition_read(targetPartition_, 0, imageHeader, sizeof(imageHeader)) != ESP_OK ||
        imageHeader[0] != 0xE9 || imageHeader[1] == 0 || imageHeader[1] > 16) {
        failAndAbort(BadImage);
        if (!persistFactOrFail(request,
            makcm::ota::snapshot::EffectPhase::ExternalEffectObserved,
            makcm::ota::snapshot::Phase::Failed,
            toNack(error),
            makcm::ota::State::Fault, error)) return false;
        return false;
    }
    if (looksLikeMergedImage(targetPartition_, imageSize_)) {
        failAndAbort(BadImage);
        if (!persistFactOrFail(request,
            makcm::ota::snapshot::EffectPhase::ExternalEffectObserved,
            makcm::ota::snapshot::Phase::Failed,
            toNack(error),
            makcm::ota::State::Fault, error)) return false;
        return false;
    }
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    if (mbedtls_sha256_starts_ret(&sha, 0) != 0) {
        mbedtls_sha256_free(&sha);
        failAndAbort(HashMismatch);
        if (!persistFactOrFail(request,
            makcm::ota::snapshot::EffectPhase::ExternalEffectObserved,
            makcm::ota::snapshot::Phase::Failed,
            toNack(error),
            makcm::ota::State::Fault, error)) return false;
        return false;
    }
    uint8_t buffer[1024] = {};
    for (uint32_t offset = 0; offset < imageSize_;) {
        const uint32_t count = std::min<uint32_t>(sizeof(buffer), imageSize_ - offset);
        if (esp_partition_read(targetPartition_, offset, buffer, count) != ESP_OK ||
            mbedtls_sha256_update_ret(&sha, buffer, count) != 0) {
            mbedtls_sha256_free(&sha);
            failAndAbort(HashMismatch);
            if (!persistFactOrFail(request,
                makcm::ota::snapshot::EffectPhase::ExternalEffectObserved,
                makcm::ota::snapshot::Phase::Failed,
                toNack(error),
                makcm::ota::State::Fault, error)) return false;
            return false;
        }
        offset += count;
    }
    uint8_t digest[32] = {};
    const bool hashOk = mbedtls_sha256_finish_ret(&sha, digest) == 0 &&
                        memcmp(digest, imageSha256_, sizeof(digest)) == 0;
    mbedtls_sha256_free(&sha);
    if (!hashOk) {
        failAndAbort(HashMismatch);
        if (!persistFactOrFail(request,
            makcm::ota::snapshot::EffectPhase::ExternalEffectObserved,
            makcm::ota::snapshot::Phase::Failed,
            toNack(error),
            makcm::ota::State::Fault, error)) return false;
        return false;
    }
    if (!verifySignature(manifestWire_, manifestLength_)) {
        failAndAbort(SignatureRequired);
        if (!persistFactOrFail(request,
            makcm::ota::snapshot::EffectPhase::ExternalEffectObserved,
            makcm::ota::snapshot::Phase::Failed,
            toNack(error),
            makcm::ota::State::Fault, error)) return false;
        return false;
    }
    lastOtaError_ = esp_ota_end(otaHandle_);
    if (lastOtaError_ != ESP_OK) {
        otaHandle_ = 0;
        error = BadImage;
        state_ = State::Failed;
        if (!persistFactOrFail(request,
            makcm::ota::snapshot::EffectPhase::ExternalEffectObserved,
            makcm::ota::snapshot::Phase::Failed,
            toNack(error),
            makcm::ota::State::Fault, error)) return false;
        return false;
    }
    otaHandle_ = 0;
    if (!verifyEmbeddedIdentity(targetPartition_, imageSize_, manifest_)) {
        error = BadImage;
        state_ = State::Failed;
        if (!persistFactOrFail(
            request, makcm::ota::snapshot::EffectPhase::ExternalEffectObserved,
            makcm::ota::snapshot::Phase::Failed,
            toNack(error),
            makcm::ota::State::Fault, error)) return false;
        return false;
    }
    state_ = State::Prepared;
    lastError_ = Ok;
    if (!persistFactOrFail(
            request, makcm::ota::snapshot::EffectPhase::ExternalEffectObserved,
            makcm::ota::snapshot::Phase::Prepared,
            makcm::ota::NackReason::None, makcm::ota::State::Prepared, error))
        return false;
    // GO-1 diagnostic crumb (remove after GO-1): whole-transfer totals.
    {
        char totals[96];
        snprintf(totals, sizeof(totals),
                 "OTA_MF T dat=%lu dup=%lu ferr=%lu ovf=%lu full=%lu frm=%lu\n",
                 static_cast<unsigned long>(g_otaDataFrames),
                 static_cast<unsigned long>(g_otaDupAccepts),
                 static_cast<unsigned long>(g_otaDecoderFrameErrors),
                 static_cast<unsigned long>(serial1RxFifoOvf),
                 static_cast<unsigned long>(serial1RxBufferFull),
                 static_cast<unsigned long>(serial1RxFrameErr));
        sendText(totals);
    }
    error = Ok;
    return true;
}

bool OtaReceiver::handleActivate(const makcm::ota::Frame &request,
                                 Error &error) {
    if (state_ != State::Prepared ||
        targetPartition_ == nullptr) {
        error = BadState;
        return false;
    }
#if !defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE) || !CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    error = RollbackUnavailable;
    return false;
#else
    if (!persistCommandIntent(request, makcm::ota::State::Prepared,
                              makcm::ota::snapshot::Phase::ActivateIntent)) {
        state_ = State::Failed;
        lastError_ = Flash;
        error = Flash;
        return false;
    }
    state_ = State::ActivateIntent;
    lastOtaError_ = esp_ota_set_boot_partition(targetPartition_);
    if (lastOtaError_ != ESP_OK ||
        esp_ota_get_boot_partition() != targetPartition_) {
        // Preserve the durable intent. A later exact Activate retry or reboot
        // can reconcile whether boot selection ever changed.
        state_ = State::ActivateIntent;
        lastError_ = Flash;
        error = Flash;
        if (!persistFactOrFail(request,
            makcm::ota::snapshot::EffectPhase::ExternalEffectObserved,
            makcm::ota::snapshot::Phase::ActivateIntent,
            makcm::ota::NackReason::FlashWriteFailed,
            makcm::ota::State::ActivationPending, error)) return false;
        return false;
    }
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    if (boot != targetPartition_) {
        state_ = State::ActivateIntent;
        lastError_ = Flash;
        error = Flash;
        if (!persistFactOrFail(request,
            makcm::ota::snapshot::EffectPhase::ExternalEffectObserved,
            makcm::ota::snapshot::Phase::ActivateIntent,
            makcm::ota::NackReason::FlashWriteFailed,
            makcm::ota::State::ActivationPending, error)) return false;
        return false;
    }
    state_ = State::Activated;
    lastError_ = Ok;
    if (!persistFactOrFail(
            request, makcm::ota::snapshot::EffectPhase::ExternalEffectObserved,
            makcm::ota::snapshot::Phase::BootSelected,
            makcm::ota::NackReason::None,
            makcm::ota::State::ActivationPending, error))
        return false;
    error = Ok;
    return true;
#endif
}

bool OtaReceiver::handleConfirm(const makcm::ota::Frame &request,
                                Error &error) {
    if (state_ != State::AwaitConfirm && state_ != State::Activated) {
        error = BadState;
        return false;
    }
    if (!peerCompatible() || !healthPassed() || !validateLocalBoot()) {
        error = BadHardware;
        return false;
    }
#if !defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE) || !CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    error = RollbackUnavailable;
    return false;
#else
    if (!persistCommandIntent(request, makcm::ota::State::PendingConfirm,
                              makcm::ota::snapshot::Phase::PairPending)) {
        error = Flash;
        return false;
    }
    state_ = State::Confirmed;
    lastError_ = Ok;
    if (!snapshotReady_) {
        error = Flash;
        return false;
    }
    error = Ok;
    return true;
#endif
}

bool OtaReceiver::validateLocalBoot() {
    if (localBootValidated_) return true;
    if (state_ != State::AwaitConfirm || !localHealthPassed()) return false;
#if defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE) && CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    if (running == nullptr || boot == nullptr || running != boot ||
        !snapshot_.active.present ||
        snapshot_.active.identity.transaction_id != transactionId_ ||
        snapshot_.active.identity.generation != kCurrentGeneration ||
        static_cast<uint8_t>(running->subtype) !=
            snapshot_.active.identity.target_slot ||
        !identityDigestMatches(snapshot_.active.identity) ||
        !partitionMatchesImage(running, snapshot_.active.identity)) return false;
    esp_ota_img_states_t preState = ESP_OTA_IMG_UNDEFINED;
    lastStateQueryError_ = esp_ota_get_state_partition(running, &preState);
    lastImageState_ = preState;
    if (lastStateQueryError_ != ESP_OK ||
        preState != ESP_OTA_IMG_PENDING_VERIFY) return false;
    markValidError_ = esp_ota_mark_app_valid_cancel_rollback();
    lastOtaError_ = markValidError_;
    if (markValidError_ != ESP_OK) return false;
    esp_ota_img_states_t postState = ESP_OTA_IMG_UNDEFINED;
    lastStateQueryError_ = esp_ota_get_state_partition(running, &postState);
    lastImageState_ = postState;
    if (lastStateQueryError_ != ESP_OK || postState != ESP_OTA_IMG_VALID)
        return false;
    localBootValidated_ = true;
    lastActivityMs_ = millis();
    return persistLocalValidationFact();
#else
    return false;
#endif
}

bool OtaReceiver::abortUpdate(Error error, bool clearPersistent) {
    bool aborted = true;
    if (otaHandle_ != 0) {
        lastOtaError_ = esp_ota_abort(otaHandle_);
        if (lastOtaError_ != ESP_OK) {
            error = Flash;
            lastError_ = Flash;
            aborted = false;
        }
        otaHandle_ = 0;
    }
    state_ = State::Failed;
    lastError_ = error;
    // Never erase V2 evidence on a timeout or an ambiguous flash result.
    // `clearPersistent` is retained only for source compatibility with the
    // old call sites and is deliberately ignored.
    (void)clearPersistent;
    haveSequence_ = false;
    hashOpen_ = false;
    return aborted;
}

void OtaReceiver::handleFrame(const makcm::ota::Frame &frame) {
    using namespace makcm::ota;
    // GO-1 diagnostic crumb (remove after GO-1): proves a frame decoded and
    // reached dispatch.  Data frames excluded -- they arrive by the hundred.
    if (frame.command != Command::Data) {
        static uint8_t frameCrumbs = 0;
        if (frameCrumbs < 5) {
            ++frameCrumbs;
            char crumb[32];
            snprintf(crumb, sizeof(crumb), "OTA_MF F c=%u\n",
                     static_cast<unsigned>(frame.command));
            sendText(crumb);
        }
    }
    replyTransactionId_ = frame.transaction_id;
    replySequence_ = frame.sequence;
    if (frame.target != Target::Right) {
        sendResponse(frame.command, BadTarget, frame.offset);
        return;
    }
    lastActivityMs_ = millis();
    const bool stateChanging =
        snapshot::ledgerIndex(frame.command) < snapshot::kLedgerCount;
    bool releaseMaintenanceAfterResponse = false;
    auto emit = [&](const Frame &response) {
        const size_t length = encodeFrame(response, wireBuffer_,
                                          sizeof(wireBuffer_));
        if (length != 0) writeWire(wireBuffer_, length);
    };
    auto releaseDurableTerminal = [&]() {
        if (snapshot_.active.present || !snapshot_.completed.present ||
            snapshot_.completed.identity.transaction_id != frame.transaction_id ||
            snapshot_.completed.terminal_command != frame.command)
            return false;
        maintenanceActive_.store(false, std::memory_order_release);
        bootstrapTransactionId_ = 0;
        replyTransactionId_ = 0;
        decoder_.reset();
        return true;
    };

    if (stateChanging) {
        memset(&responseFrame_, 0, sizeof(responseFrame_));
        const ReplayDecision replay = replayCommand(frame, responseFrame_);
        if (replay == ReplayDecision::Replayed) {
            emit(responseFrame_);
            if (frame.command == Command::Abort ||
                frame.command == Command::Confirm)
                (void)releaseDurableTerminal();
            return;
        }
        if (replay == ReplayDecision::ResumeIntent ||
            replay == ReplayDecision::ResumeObserved) {
            Error resumeError = Ok;
            const bool resumed = resumeIncompleteCommand(
                frame, replay, responseFrame_, resumeError);
            if (responseFrame_.length == 0)
                fillResponse(frame, resumed ? Command::Ack : Command::Nack,
                             toNack(resumeError), responseFrame_);
            emit(responseFrame_);
            if (resumed && (frame.command == Command::Abort ||
                            frame.command == Command::Confirm))
                (void)releaseDurableTerminal();
            if (resumed && frame.command == Command::Activate) {
                vTaskDelay(pdMS_TO_TICKS(100));
                esp_restart();
            }
            return;
        }
        if (replay == ReplayDecision::Conflict) {
            fillResponse(frame, Command::Nack, NackReason::WrongTransaction,
                         responseFrame_);
            emit(responseFrame_);
            return;
        }
    }

    Error error = Ok;
    bool accepted = false;
    bool restart = false;
    switch (frame.command) {
    case Command::Hello:
        sendStatus(frame);
        return;
    case Command::EnterMaintenance:
        accepted = enterMaintenance(frame.transaction_id, peerGeneration_);
        if (!accepted) error = BadState;
        break;
    case Command::Manifest: {
        if (frame.transaction_id == 0 || frame.offset != 0 ||
            (bootstrapTransactionId_ != 0 &&
             frame.transaction_id != bootstrapTransactionId_) ||
             (state_ != State::Idle && state_ != State::Failed)) {
            error = BadState;
            break;
        }
        // A failed/ambiguous active V2 transaction is still evidence.  Do not
        // let a different transaction overwrite its identity merely because
        // the volatile state is Failed; only an exact replay may reconcile it.
        if (snapshot_.active.present &&
            frame.transaction_id != snapshot_.active.identity.transaction_id) {
            error = BadTransaction;
            break;
        }
        {
            // Breadcrumbs for the one-shot manifest path: it hung silently on
            // hardware before any durable intent existed, so localize via the
            // maintenance text plane.  hw = stack high-water mark in words.
            char crumb[48];
            snprintf(crumb, sizeof(crumb), "OTA_MF A hw=%u\n",
                     static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
            sendText(crumb);
        }
        if (!validateManifest(frame.payload, frame.length, error)) break;
        {
            char crumb[48];
            snprintf(crumb, sizeof(crumb), "OTA_MF B hw=%u\n",
                     static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
            sendText(crumb);
        }
        transactionId_ = frame.transaction_id;
        bytesWritten_ = 0;
        lastError_ = Ok;
        lastDataOffset_ = 0;
        lastDataLength_ = 0;
        lastDataCrc_ = 0;
        lastDataSequence_ = 0;
        memset(lastDataSha256_, 0, sizeof(lastDataSha256_));
        auto &nextSnapshot = beginSnapshotMutation();
        if (!buildActiveIdentity(frame) ||
            !setLedgerIntent(nextSnapshot.active, frame,
                             makcm::ota::State::Idle,
                             snapshot::Phase::Receiving) ||
            !commitPendingSnapshotV2()) {
            error = Flash;
            state_ = State::Failed;
            break;
        }
        sendText("OTA_MF C\n");
        lastOtaError_ = esp_ota_begin(targetPartition_, imageSize_, &otaHandle_);
        sendText("OTA_MF D\n");
        if (lastOtaError_ != ESP_OK) {
            otaHandle_ = 0;
            error = Flash;
            state_ = State::Failed;
            if (!persistFactOrFail(frame, snapshot::EffectPhase::ExternalEffectObserved,
                                   snapshot::Phase::Failed,
                                   NackReason::FlashWriteFailed,
                                   makcm::ota::State::Fault, error)) {
                /* helper has converted the response to FlashWriteFailed */
            }
            break;
        }
        hashOpen_ = mbedtls_sha256_starts_ret(&imageHash_, 0) == 0;
        if (!hashOpen_) {
            lastError_ = HashMismatch;
            error = HashMismatch;
            state_ = State::Failed;
            if (!persistFactOrFail(frame, snapshot::EffectPhase::ExternalEffectObserved,
                                   snapshot::Phase::Failed,
                                   NackReason::ImageVerificationFailed,
                                   makcm::ota::State::Fault, error)) {
                /* helper has converted the response to FlashWriteFailed */
            }
            break;
        }
        state_ = State::Receiving;
        if (!persistFactOrFail(frame,
                               snapshot::EffectPhase::ExternalEffectObserved,
                               snapshot::Phase::Receiving,
                               NackReason::None,
                               makcm::ota::State::Receiving, error)) {
            /* helper leaves the durable intent and records Failed/Flash */
        } else {
            accepted = true;
        }
        break;
    }
    case Command::Data:
        if (frame.transaction_id != transactionId_) {
            error = BadTransaction;
            break;
        }
        if (frame.sequence == 0 || frame.sequence == UINT32_MAX ||
            (haveSequence_ && (frame.flags & kFlagRetransmit) == 0 &&
             frame.sequence <= lastSequence_)) {
            error = BadOffset;
            break;
        }
        accepted = handleData(frame.sequence, frame.offset, frame.payload,
                              frame.length, error);
        if (accepted && frame.offset >= bytesWritten_ - frame.length) {
            lastSequence_ = frame.sequence;
            haveSequence_ = true;
            // DATA progress is deliberately RAM-only.  A reboot destroys the
            // esp_ota handle and the streaming SHA context, so restore() always
            // requires the exact signed MANIFEST to be replayed from offset 0.
            // Committing a snapshot per chunk therefore buys no resumability
            // and only adds flash wear plus an NVS failure window per block.
        }
        break;
    case Command::Prepare:
        if (frame.transaction_id != transactionId_) {
            error = BadTransaction;
            break;
        }
        accepted = prepareImage(frame, error);
        break;
    case Command::Status:
        sendStatus(frame);
        // A status bootstrap while there is no update transaction is also an
        // explicit maintenance probe.  Report the common Maintenance state
        // first (so the host can verify peer/health flags), then return UART1
        // to the normal bridge.  Prepared/Receiving/AwaitConfirm never take
        // this path and therefore retain exclusive binary ownership.
        if (state_ == State::Idle && transactionId_ == 0) {
            maintenanceActive_ = false;
            bootstrapTransactionId_ = 0;
            decoder_.reset();
        }
        return;
    case Command::Activate:
        if (frame.transaction_id != transactionId_) {
            error = BadTransaction;
            break;
        }
        accepted = handleActivate(frame, error);
        restart = accepted;
        break;
    case Command::Abort:
        if (frame.transaction_id != 0 && transactionId_ != 0 &&
            frame.transaction_id != transactionId_) {
            error = BadTransaction;
            break;
        }

        if (state_ == State::Idle && maintenanceActive_ &&
            !snapshot_.active.present && transactionId_ == 0 &&
            (bootstrapTransactionId_ == 0 ||
             frame.transaction_id == bootstrapTransactionId_)) {
            // ENTER_MAINTENANCE before MANIFEST is only a transient UART/USB
            // ownership session.  No partition, boot selector, or durable
            // image identity exists, so cancellation must not require (or
            // fabricate) a V2 rollback ledger.
            releaseMaintenanceAfterResponse = true;
            accepted = true;
            restart = false;
            break;
        }

        // Once ACTIVATE has selected the new slot and the board has rebooted,
        // loadPersistentState() exposes that durable evidence as AwaitConfirm.
        // An explicit ABORT in this state must use the bootloader rollback
        // path.  Clearing Preferences first would make the reboot look like a
        // fresh Idle receiver and leave the pending image selected instead of
        // returning to the previous slot.  Emit the ACK while the evidence is
        // still intact, then ask the bootloader to invalidate this image.
        if (state_ == State::ActivateIntent) {
            // Boot selection may already point at the target even when the
            // post-selection fact was not persisted. Never erase intent in
            // this ambiguous state.
            error = RollbackUnavailable;
            lastError_ = RollbackUnavailable;
            break;
        }
        if (state_ == State::AwaitConfirm || state_ == State::Activated) {
#if defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE) && CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
            // V1 does not persist the exact previous-slot identity. Once this
            // image is VALID, a write-target selection API must not be guessed
            // as "previous". Refuse safely.
            if (localBootValidated_) {
                error = RollbackUnavailable;
                lastError_ = RollbackUnavailable;
                /* V2 intent remains authoritative; do not clear it. */
                break;
            }
            if (!persistCommandIntent(frame, makcm::ota::State::PendingConfirm,
                                      snapshot::Phase::RollbackIntent)) {
                error = Flash;
                break;
            }
            sendResponse(frame.command, Ok, frame.offset);
            vTaskDelay(pdMS_TO_TICKS(100));
            lastOtaError_ = esp_ota_mark_app_invalid_rollback_and_reboot();
            if (lastOtaError_ != ESP_OK) {
                // The rollback API normally does not return on success.  If
                // it does return, preserve the transaction evidence so a
                // later maintenance attempt can diagnose/retry safely.
                lastError_ = Flash;
                if (!persistFactOrFail(frame, snapshot::EffectPhase::ExternalEffectObserved,
                                       snapshot::Phase::Failed,
                                       NackReason::FlashWriteFailed,
                                       makcm::ota::State::Fault, error)) {
                    /* helper has converted the response to FlashWriteFailed */
                }
            }
            return;
#else
            error = RollbackUnavailable;
            break;
#endif
        }
        if (snapshot_.active.present &&
            snapshot_.active.identity.transaction_id == frame.transaction_id) {
            if (!persistCommandIntent(frame, state_ == State::Prepared
                                             ? makcm::ota::State::Prepared
                                             : makcm::ota::State::Receiving,
                                       snapshot::Phase::RollbackIntent)) {
                error = Flash;
                break;
            }
            if (!abortUpdate(Ok, false)) {
                error = Flash;
                state_ = State::Failed;
                break;
            }
            state_ = State::Idle;
            releaseMaintenanceAfterResponse = true;
            accepted = true;
            // Receiving/Prepared abort never changes the boot selector.
            restart = false;
        } else {
            error = BadTransaction;
        }
        break;
    case Command::Confirm:
        if (frame.transaction_id != 0 && transactionId_ != 0 &&
            frame.transaction_id != transactionId_) {
            error = BadTransaction;
            break;
        }
        if (state_ == State::Confirmed ||
            (state_ == State::Idle &&
             completedMatches(frame.transaction_id))) {
            // Exact replay after a lost ACK returns the durable completed fact
            // without another flash write or reboot.
            accepted = true;
            restart = false;
        } else {
            accepted = handleConfirm(frame, error);
            restart = accepted;
        }
        break;
    default:
        error = Unsupported;
        break;
    }
    if (!accepted && error == Ok) error = BadState;
    memset(&responseFrame_, 0, sizeof(responseFrame_));
    fillResponse(frame, accepted ?
                 (frame.command == Command::Status ? Command::StatusResponse
                                                    : Command::Ack)
                 : Command::Nack,
                 toNack(error), responseFrame_);
    if (stateChanging && snapshot_.active.present &&
        snapshot_.active.identity.transaction_id == frame.transaction_id) {
        if (!finalizeCommandResponse(frame, responseFrame_, toNack(error),
                                     accepted)) {
            error = Flash;
            accepted = false;
            releaseMaintenanceAfterResponse = false;
            restart = false;
            fillResponse(frame, Command::Nack, NackReason::FlashWriteFailed,
                         responseFrame_);
        }
    }
    emit(responseFrame_);
    const bool transientCancel = releaseMaintenanceAfterResponse &&
        !snapshot_.active.present && !snapshot_.completed.present &&
        transactionId_ == 0;
    if (releaseMaintenanceAfterResponse &&
        (transientCancel || releaseDurableTerminal())) {
        maintenanceActive_.store(false, std::memory_order_release);
        bootstrapTransactionId_ = 0;
        replyTransactionId_ = 0;
        decoder_.reset();
    }
    if (restart) {
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
    }
}

void OtaReceiver::feed(uint8_t byte) {
    if (!maintenanceActive_) return;
    lastActivityMs_ = millis();
    // GO-1 diagnostic crumb (remove after GO-1): first wire byte reaching the
    // OTA decoder this boot.  Distinguishes "bytes never routed here" from
    // "decoder hunting magic forever", which are both otherwise silent.
    static bool firstByteCrumb = false;
    if (!firstByteCrumb) {
        firstByteCrumb = true;
        sendText("OTA_MF R\n");
    }
    const auto result = decoder_.feed(byte, requestFrame_);
    if (result == makcm::ota::FeedResult::FrameReady) {
        handleFrame(requestFrame_);
    } else if (result == makcm::ota::FeedResult::FrameError) {
        ++g_otaDecoderFrameErrors;
        lastError_ = BadFrame;
        // GO-1 diagnostic crumb (remove after GO-1): a frame died in the
        // decoder.  This path was fully silent, which made a CRC/framing
        // mismatch indistinguishable from bytes never arriving.
        static uint8_t frameErrorCrumbs = 0;
        if (frameErrorCrumbs < 3) {
            ++frameErrorCrumbs;
            sendText("OTA_MF E\n");
        }
    }
}

void OtaReceiver::tick() {
    if (state_ == State::AwaitConfirm && !localBootValidated_) {
        (void)validateLocalBoot();
    }
    if (state_ == State::AwaitConfirm && !localBootValidated_ &&
        static_cast<uint32_t>(millis() - lastActivityMs_) > kConfirmTimeoutMs) {
        // Host/pair silence is not permission to kill a locally bootable
        // image. Preserve the record and remain fail-closed for reconciliation.
        state_ = State::Failed;
        lastError_ = Timeout;
        persistState();
        return;
    }
    if (!maintenanceActive_) return;
    // Only an interrupted download is an idle-timeout condition. Prepared is
    // a durable two-phase-commit state: the host must be allowed to inspect
    // both boards and issue ACTIVATE later without the right board silently
    // discarding the verified slot after ten seconds.
    if (state_ == State::Receiving &&
        static_cast<uint32_t>(millis() - lastActivityMs_) > kIdleTimeoutMs) {
        abortUpdate(Timeout);
        sendResponse(makcm::ota::Command::Abort, Timeout);
        lastActivityMs_ = millis();
    }
}
