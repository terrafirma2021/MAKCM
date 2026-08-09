#include "OtaReceiver.h"

#include <algorithm>
#include <esp_app_format.h>
#include <esp_log.h>
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
constexpr uint32_t kCurrentGeneration =
    makcm::ota::build::kPairGeneration;
static uint8_t s_compareBuffer[makcm::ota::kMaxChunkSize]{};

bool exactFixedString(const char *expected, const char *actual,
                      size_t capacity) {
    if (expected == nullptr || actual == nullptr || capacity < 2) return false;
    const size_t length = strlen(expected);
    return length != 0 && length < capacity &&
           memcmp(actual, expected, length) == 0 && actual[length] == '\0';
}

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
        "makcm_right_idf",
        MAKCM_OTA_PAIR_APP_VERSION,
        {'I', 'D', 'E', 'N', 'D', 'V', '1', '\0'},
};

bool markerLinked() {
    volatile const uint8_t *raw = reinterpret_cast<volatile const uint8_t *>(
        &g_makcm_right_image_identity);
    makcm::ota::EmbeddedImageIdentity copy{};
    for (size_t i = 0; i < sizeof(copy); ++i)
        reinterpret_cast<uint8_t *>(&copy)[i] = raw[i];
    return makcm::ota::validEmbeddedImageIdentity(copy) &&
           copy.role == static_cast<uint8_t>(makcm::ota::Target::Right) &&
           copy.generation == kCurrentGeneration &&
           copy.partition_schema ==
               makcm::ota::board::kRightPartitionSchemaV1;
}

bool verifyEmbeddedIdentity(const esp_partition_t *partition,
                            uint32_t imageSize,
                            const makcm::ota::Manifest &manifest) {
    using namespace makcm::ota;
    if (partition == nullptr || imageSize < sizeof(EmbeddedImageIdentity) ||
        imageSize > partition->size)
        return false;
    const size_t overlap = sizeof(EmbeddedImageIdentity) - 1;
    uint8_t found = 0;
    for (uint32_t base = 0; base < imageSize;) {
        const uint32_t count = std::min<uint32_t>(
            sizeof(s_compareBuffer), imageSize - base);
        if (esp_partition_read(partition, base, s_compareBuffer, count) !=
            ESP_OK)
            return false;
        for (uint32_t i = 0; i + sizeof(EmbeddedImageIdentity) <= count; ++i) {
            if (memcmp(s_compareBuffer + i, kEmbeddedIdentityMagic,
                       sizeof(kEmbeddedIdentityMagic)) != 0)
                continue;
            EmbeddedImageIdentity identity{};
            memcpy(&identity, s_compareBuffer + i, sizeof(identity));
            if (!validEmbeddedImageIdentity(identity)) continue;
            char expectedVersion[32]{};
            snprintf(expectedVersion, sizeof(expectedVersion), "makcm-g%lu",
                     static_cast<unsigned long>(manifest.generation));
            if (identity.role != static_cast<uint8_t>(manifest.role) ||
                identity.generation != manifest.generation ||
                identity.partition_schema != manifest.partition_schema ||
                identity.protocol_version != manifest.protocol_version ||
                !exactFixedString(board::kHardwareRevisionR1,
                                  identity.hardware_revision,
                                  sizeof(identity.hardware_revision)) ||
                !exactFixedString(board::kRightProjectName,
                                  identity.project_name,
                                  sizeof(identity.project_name)) ||
                !exactFixedString(expectedVersion, identity.app_version,
                                  sizeof(identity.app_version)))
                continue;
            if (++found > 1) return false;
        }
        if (count == imageSize - base) break;
        base += count > overlap ? count - static_cast<uint32_t>(overlap) : 1;
    }
    return found == 1;
}

bool isOtaApplication(const esp_partition_t *partition) {
    return partition != nullptr && partition->type == ESP_PARTITION_TYPE_APP &&
           (partition->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 ||
            partition->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1);
}

bool looksLikeMergedImage(const esp_partition_t *partition,
                          uint32_t imageSize) {
    if (partition == nullptr) return true;
    uint8_t bytes[2]{};
    for (const uint32_t offset : {0x8000u, 0x9000u}) {
        if (offset + sizeof(bytes) <= imageSize &&
            esp_partition_read(partition, offset, bytes, sizeof(bytes)) ==
                ESP_OK &&
            bytes[0] == 0xAA && bytes[1] == 0x50)
            return true;
    }
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
    case OtaReceiver::BadRole: return NackReason::WrongTarget;
    case OtaReceiver::BadHardware: return NackReason::CompatibilityRejected;
    case OtaReceiver::BadManifest:
    case OtaReceiver::BadPartition:
    case OtaReceiver::BadImage: return NackReason::ManifestRejected;
    case OtaReceiver::BadOffset: return NackReason::WrongOffset;
    case OtaReceiver::BadCrc: return NackReason::CrcMismatch;
    case OtaReceiver::Timeout: return NackReason::Timeout;
    case OtaReceiver::HashMismatch:
        return NackReason::ImageVerificationFailed;
    case OtaReceiver::SignatureRequired:
        return NackReason::SignatureVerificationFailed;
    case OtaReceiver::RollbackUnavailable:
        return NackReason::RollbackUnavailable;
    case OtaReceiver::Flash: return NackReason::FlashWriteFailed;
    case OtaReceiver::Unsupported: return NackReason::InvalidState;
    }
    return NackReason::Malformed;
}

uint32_t statusFlags(const OtaReceiver &receiver) {
    uint32_t flags = receiver.isMaintenanceActive()
                         ? static_cast<uint32_t>(
                               makcm::ota::kStatusMaintenanceExclusive)
                         : 0U;
#if defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE) && \
    CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    flags |= makcm::ota::kStatusRollbackSupported;
#endif
    if (makcm::ota::trust::configured())
        flags |= makcm::ota::kStatusTrustConfigured;
    if (receiver.peerCompatible())
        flags |= makcm::ota::kStatusPeerCompatible;
    if (receiver.healthPassed()) flags |= makcm::ota::kStatusHealthPassed;
    if (receiver.localHealthPassed())
        flags |= makcm::ota::kStatusLocalBootValidated;
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
    auto &pointX = point.MBEDTLS_PRIVATE(X);
    auto &pointY = point.MBEDTLS_PRIVATE(Y);
    auto &pointZ = point.MBEDTLS_PRIVATE(Z);
#else
    auto &group = context.grp;
    auto &point = context.Q;
    auto &pointX = point.X;
    auto &pointY = point.Y;
    auto &pointZ = point.Z;
#endif
    bool ok = mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1) == 0;
    ok = ok && mbedtls_mpi_read_binary(&pointX, kPublicX, sizeof(kPublicX)) == 0;
    ok = ok && mbedtls_mpi_read_binary(&pointY, kPublicY, sizeof(kPublicY)) == 0;
    ok = ok && mbedtls_mpi_lset(&pointZ, 1) == 0;
    ok = ok && mbedtls_mpi_read_binary(&r, signature, 32) == 0;
    ok = ok && mbedtls_mpi_read_binary(&s, signature + 32, 32) == 0;
    ok = ok && mbedtls_ecdsa_verify(&group, digest, 32, &point, &r, &s) == 0;
    mbedtls_mpi_free(&s);
    mbedtls_mpi_free(&r);
    mbedtls_ecdsa_free(&context);
    return ok;
}
} // namespace

OtaReceiver otaReceiver;

void OtaReceiver::begin(HardwareSerial *serial, SemaphoreHandle_t *mutex,
                        LocalHealthCheck localHealthCheck,
                        PeerHealthCheck peerHealthCheck,
                        void *healthContext) {
    serial_ = serial;
    serialMutex_ = mutex;
    localHealthCheck_ = localHealthCheck;
    peerHealthCheck_ = peerHealthCheck;
    healthContext_ = healthContext;
    maintenanceActive_.store(false, std::memory_order_release);
    decoder_.reset();
    clearResponseCache();
    resetReason_ = esp_reset_reason();
    state_ = State::Idle;
    lastError_ = Ok;
    initialized_ = serial_ != nullptr && serialMutex_ != nullptr &&
                   *serialMutex_ != nullptr && markerLinked();
    if (!initialized_) {
        lastError_ = serial_ == nullptr || serialMutex_ == nullptr ||
                             *serialMutex_ == nullptr
                         ? Flash
                         : BadImage;
        ESP_LOGE(kTag, "OTA receiver initialization failed");
        return;
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    lastImageState_ = ESP_OTA_IMG_UNDEFINED;
    lastStateQueryError_ = running == nullptr
                               ? ESP_ERR_NOT_FOUND
                               : esp_ota_get_state_partition(running,
                                                             &lastImageState_);
    runningImageSha256Valid_ =
        running != nullptr &&
        esp_partition_get_sha256(running, runningImageSha256_) == ESP_OK;
    state_ = lastStateQueryError_ == ESP_OK &&
                     lastImageState_ == ESP_OTA_IMG_PENDING_VERIFY
                 ? State::PendingConfirm
                 : State::Idle;
    lastActivityMs_ = millis();
    pendingConfirmStartedMs_ =
        state_ == State::PendingConfirm ? lastActivityMs_ : 0;
}

bool OtaReceiver::enterMaintenance(uint32_t transactionId,
                                   uint32_t peerGeneration) {
    lastMaintenanceReject_ = MaintenanceReject::None;
    if (!initialized_) {
        lastMaintenanceReject_ = MaintenanceReject::NotInitialized;
        return false;
    }
    if (transactionId == 0) {
        lastMaintenanceReject_ = MaintenanceReject::ZeroTransaction;
        return false;
    }
    if (isMaintenanceActive()) {
        // The explicit port lock makes the host single-owner. Rebinding only
        // the volatile maintenance session lets a restarted host recover a
        // preserved Receiving/Prepared image transaction without a board
        // reset; image-mutating commands still validate transactionId_.
        sessionTransactionId_ = transactionId;
        if (peerGeneration != 0) peerGeneration_ = peerGeneration;
        lastActivityMs_ = millis();
        decoder_.reset();
        clearResponseCache();
        return true;
    }
    if (xSemaphoreTake(*serialMutex_, pdMS_TO_TICKS(100)) != pdTRUE) {
        lastMaintenanceReject_ = MaintenanceReject::SerialBusy;
        return false;
    }
    maintenanceActive_.store(true, std::memory_order_release);
    sessionTransactionId_ = transactionId;
    if (peerGeneration != 0) peerGeneration_ = peerGeneration;
    lastActivityMs_ = millis();
    decoder_.reset();
    clearResponseCache();
    xSemaphoreGive(*serialMutex_);
    return true;
}

bool OtaReceiver::peerCompatible() const {
    if (peerGeneration_ == 0) return false;
    if (state_ == State::Receiving || state_ == State::Prepared)
        return peerGeneration_ >= minPeerGeneration_ &&
               peerGeneration_ <= maxPeerGeneration_;
    const uint32_t oldestRollingPeer =
        kCurrentGeneration > 1 ? kCurrentGeneration - 1 : 1;
    return peerGeneration_ >= oldestRollingPeer &&
           peerGeneration_ <= kCurrentGeneration;
}

bool OtaReceiver::sendText(const char *text) {
    return text != nullptr &&
           writeWire(reinterpret_cast<const uint8_t *>(text), strlen(text));
}

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
                               makcm::ota::Command command,
                               makcm::ota::NackReason reason,
                               makcm::ota::Frame &response) const {
    using namespace makcm::ota;
    memset(&response, 0, sizeof(response));
    response.target = Target::Right;
    response.command = command;
    response.flags = kFlagResponse;
    response.transaction_id = request.transaction_id;
    response.offset = state_ == State::Receiving || state_ == State::Prepared
                          ? bytesWritten_
                          : 0;
    response.sequence = request.sequence;
    CommandResponsePayload result{};
    result.state = state_;
    result.last_command = request.command;
    result.reason = reason;
    result.next_offset = response.offset;
    result.target_generation =
        state_ == State::Receiving || state_ == State::Prepared
            ? targetGeneration_
            : 0;
    result.active_generation = kCurrentGeneration;
    result.status_flags = statusFlags(*this);
    encodeCommandResponsePayload(result, response.payload);
    response.length = kCommandResponsePayloadSize;
}

bool OtaReceiver::fillStatusV2(const makcm::ota::Frame &request,
                               makcm::ota::Frame &response) {
    using namespace makcm::ota;
    if (request.command != Command::Status || request.length != 0 ||
        (request.flags & ~kFlagRetransmit) != kFlagStatusV2)
        return false;

    StatusPayloadV2 status{};
    status.role = Target::Right;
    status.state = state_;
    status.last_command = Command::Status;
    status.last_response_command = Command::StatusResponse;
    status.reason = NackReason::None;
    status.status_flags = statusFlags(*this);
    status.running_generation = kCurrentGeneration;
    status.partition_schema = makcm::ota::board::kRightPartitionSchemaV1;
    status.reset_reason = static_cast<uint32_t>(resetReason_);
    status.last_ota_error = static_cast<int32_t>(lastOtaError_);
    memcpy(status.hardware_revision,
           g_makcm_right_image_identity.hardware_revision,
           sizeof(status.hardware_revision));
    memcpy(status.project_name, g_makcm_right_image_identity.project_name,
           sizeof(status.project_name));
    memcpy(status.app_version, g_makcm_right_image_identity.app_version,
           sizeof(status.app_version));

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    esp_ota_img_states_t imageState = ESP_OTA_IMG_UNDEFINED;
    lastStateQueryError_ = running == nullptr || boot == nullptr
                               ? ESP_ERR_NOT_FOUND
                               : esp_ota_get_state_partition(running,
                                                             &imageState);
    status.last_state_query_error =
        static_cast<int32_t>(lastStateQueryError_);
    if (lastStateQueryError_ == ESP_OK) {
        lastImageState_ = imageState;
        status.running_slot = static_cast<uint8_t>(running->subtype);
        status.boot_slot = static_cast<uint8_t>(boot->subtype);
        status.image_state = static_cast<uint8_t>(imageState);
    }
    if (!runningImageSha256Valid_) return false;
    memcpy(status.running_image_sha256, runningImageSha256_,
           sizeof(status.running_image_sha256));

    if (state_ == State::Receiving || state_ == State::Prepared) {
        status.next_offset = bytesWritten_;
        status.transaction_id = transactionId_;
        status.target_generation = targetGeneration_;
        status.image_size = imageSize_;
        status.target_slot = targetSubtype_;
        memcpy(status.target_image_sha256, imageSha256_,
               sizeof(status.target_image_sha256));
        memcpy(status.manifest_sha256, manifestSha256_,
               sizeof(status.manifest_sha256));
    }
    if (!encodeStatusV2(status, statusV2Wire_, sizeof(statusV2Wire_)))
        return false;

    memset(&response, 0, sizeof(response));
    response.target = Target::Right;
    response.command = Command::StatusResponse;
    response.flags = kFlagResponse | kFlagStatusV2;
    response.transaction_id = request.transaction_id;
    response.offset = status.next_offset;
    response.sequence = request.sequence;
    response.length = kStatusV2PayloadSize;
    memcpy(response.payload, statusV2Wire_, kStatusV2PayloadSize);
    return true;
}

void OtaReceiver::cacheRequest(const makcm::ota::Frame &request) {
    requestCache_.target = request.target;
    requestCache_.command = request.command;
    requestCache_.flags = request.flags & ~makcm::ota::kFlagRetransmit;
    requestCache_.transactionId = request.transaction_id;
    requestCache_.offset = request.offset;
    requestCache_.sequence = request.sequence;
    requestCache_.length = request.length;
    if (mbedtls_sha256(request.payload, request.length,
                       requestCache_.payloadSha256, 0) != 0)
        memset(requestCache_.payloadSha256, 0,
               sizeof(requestCache_.payloadSha256));
}

bool OtaReceiver::requestMatchesCache(
    const makcm::ota::Frame &request) const {
    if (!responseCacheValid_ ||
        (request.flags & makcm::ota::kFlagRetransmit) == 0 ||
        request.target != requestCache_.target ||
        request.command != requestCache_.command ||
        (request.flags & ~makcm::ota::kFlagRetransmit) != requestCache_.flags ||
        request.transaction_id != requestCache_.transactionId ||
        request.offset != requestCache_.offset ||
        request.sequence != requestCache_.sequence ||
        request.length != requestCache_.length)
        return false;
    uint8_t digest[32]{};
    return mbedtls_sha256(request.payload, request.length, digest, 0) == 0 &&
           memcmp(digest, requestCache_.payloadSha256, sizeof(digest)) == 0;
}

bool OtaReceiver::emitResponse(const makcm::ota::Frame &request,
                               const makcm::ota::Frame &response,
                               bool cache) {
    const size_t wireLength = makcm::ota::encodeFrame(
        response, wireBuffer_, sizeof(wireBuffer_));
    if (wireLength == 0 || !writeWire(wireBuffer_, wireLength)) return false;
    if (cache && wireLength <= sizeof(responseCacheWire_)) {
        cacheRequest(request);
        memcpy(responseCacheWire_, wireBuffer_, wireLength);
        responseCacheLength_ = wireLength;
        responseCacheValid_ = true;
    }
    return true;
}

void OtaReceiver::clearResponseCache() {
    responseCacheValid_ = false;
    responseCacheLength_ = 0;
    memset(&requestCache_, 0, sizeof(requestCache_));
}

void OtaReceiver::releaseMaintenance() {
    maintenanceActive_.store(false, std::memory_order_release);
    sessionTransactionId_ = 0;
    peerGeneration_ = 0;
    decoder_.reset();
    clearResponseCache();
}

void OtaReceiver::resetTransient() {
    state_ = State::Idle;
    pendingConfirmStartedMs_ = 0;
    transactionId_ = 0;
    targetGeneration_ = 0;
    minPeerGeneration_ = 0;
    maxPeerGeneration_ = 0;
    imageSize_ = 0;
    bytesWritten_ = 0;
    targetSubtype_ = 0xFF;
    targetPartition_ = nullptr;
    memset(imageSha256_, 0, sizeof(imageSha256_));
    memset(manifestSha256_, 0, sizeof(manifestSha256_));
    memset(manifestWire_, 0, sizeof(manifestWire_));
    manifest_ = {};
}

bool OtaReceiver::abortUpdate(Error error) {
    bool ok = true;
    if (otaHandle_ != 0) {
        lastOtaError_ = esp_ota_abort(otaHandle_);
        otaHandle_ = 0;
        if (lastOtaError_ != ESP_OK) {
            error = Flash;
            ok = false;
        }
    }
    resetTransient();
    lastError_ = error;
    return ok;
}

bool OtaReceiver::validateManifest(const uint8_t *payload, uint16_t length,
                                   Error &error) {
    using namespace makcm::ota;
    Manifest candidate{};
    if (payload == nullptr || length != kManifestWireSize ||
        !decodeManifest(payload, length, candidate)) {
        error = BadManifest;
        return false;
    }
    if (candidate.role != Target::Right ||
        candidate.image_kind != kImageKindApplication) {
        error = BadRole;
        return false;
    }
    if (candidate.protocol_version != board::kPairProtocolVersion) {
        error = BadVersion;
        return false;
    }
    if (candidate.partition_schema != board::kRightPartitionSchemaV1) {
        error = BadPartition;
        return false;
    }
    if (strcmp(candidate.hardware_revision, board::kHardwareRevisionR1) != 0 ||
        strcmp(candidate.project_name, board::kRightProjectName) != 0 ||
        candidate.generation <= kCurrentGeneration ||
        candidate.min_peer_generation == 0 ||
        candidate.max_peer_generation == 0 || peerGeneration_ == 0 ||
        !manifestCompatibleWithPeer(candidate, peerGeneration_)) {
        error = BadHardware;
        return false;
    }
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *target = esp_ota_get_next_update_partition(running);
    if (!isOtaApplication(target) || target == running ||
        candidate.image_size < 32 || candidate.image_size > target->size) {
        error = BadPartition;
        return false;
    }
    if (!verifySignature(payload, length)) {
        error = SignatureRequired;
        return false;
    }
    uint8_t manifestDigest[32]{};
    if (mbedtls_sha256(payload, length, manifestDigest, 0) != 0) {
        error = HashMismatch;
        return false;
    }

    manifest_ = candidate;
    targetPartition_ = target;
    targetSubtype_ = static_cast<uint8_t>(target->subtype);
    targetGeneration_ = candidate.generation;
    minPeerGeneration_ = candidate.min_peer_generation;
    maxPeerGeneration_ = candidate.max_peer_generation;
    imageSize_ = candidate.image_size;
    memcpy(imageSha256_, candidate.image_sha256, sizeof(imageSha256_));
    memcpy(manifestSha256_, manifestDigest, sizeof(manifestSha256_));
    memcpy(manifestWire_, payload, sizeof(manifestWire_));
    error = Ok;
    return true;
}

bool OtaReceiver::verifySignature(const uint8_t *wire, uint16_t length) const {
    if (wire == nullptr || length != makcm::ota::kManifestWireSize ||
        memcmp(wire + 112, makcm::ota::trust::kSignerKeyId,
               sizeof(makcm::ota::trust::kSignerKeyId)) != 0)
        return false;
    uint8_t digest[32]{};
    return mbedtls_sha256(wire, makcm::ota::kManifestSignedSize, digest, 0) ==
               0 &&
           verifyP256(digest, wire + makcm::ota::kManifestSignedSize);
}

bool OtaReceiver::handleData(uint32_t offset, const uint8_t *payload,
                             uint16_t length, Error &error) {
    if (state_ != State::Receiving || otaHandle_ == 0 || payload == nullptr ||
        length == 0 || length > makcm::ota::kMaxChunkSize ||
        offset != bytesWritten_ || offset > imageSize_ ||
        length > imageSize_ - offset ||
        (length < makcm::ota::kMinChunkSize && offset + length != imageSize_)) {
        error = offset != bytesWritten_ ? BadOffset : BadState;
        return false;
    }
    if (offset == 0) {
        const size_t descriptorOffset = sizeof(esp_image_header_t) +
                                        sizeof(esp_image_segment_header_t);
        if (length < descriptorOffset + sizeof(esp_app_desc_t) ||
            payload[0] != ESP_IMAGE_HEADER_MAGIC) {
            error = BadImage;
            return false;
        }
        esp_app_desc_t descriptor{};
        memcpy(&descriptor, payload + descriptorOffset, sizeof(descriptor));
        if (descriptor.magic_word != ESP_APP_DESC_MAGIC_WORD ||
            strcmp(descriptor.project_name,
                   makcm::ota::board::kRightProjectName) != 0) {
            error = BadImage;
            return false;
        }
    }
    lastOtaError_ = esp_ota_write(otaHandle_, payload, length);
    if (lastOtaError_ != ESP_OK) {
        error = Flash;
        return false;
    }
    bytesWritten_ += length;
    error = Ok;
    return true;
}

bool OtaReceiver::prepareImage(Error &error) {
    if (state_ != State::Receiving || otaHandle_ == 0 ||
        targetPartition_ == nullptr || bytesWritten_ != imageSize_) {
        error = BadState;
        return false;
    }
    uint8_t imageHeader[4]{};
    if (esp_partition_read(targetPartition_, 0, imageHeader,
                           sizeof(imageHeader)) != ESP_OK ||
        imageHeader[0] != ESP_IMAGE_HEADER_MAGIC || imageHeader[1] == 0 ||
        imageHeader[1] > 16 ||
        looksLikeMergedImage(targetPartition_, imageSize_)) {
        error = BadImage;
        return false;
    }

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    if (mbedtls_sha256_starts(&sha, 0) != 0) {
        mbedtls_sha256_free(&sha);
        error = HashMismatch;
        return false;
    }
    uint8_t buffer[1024]{};
    bool readOk = true;
    for (uint32_t offset = 0; offset < imageSize_;) {
        const uint32_t count =
            std::min<uint32_t>(sizeof(buffer), imageSize_ - offset);
        if (esp_partition_read(targetPartition_, offset, buffer, count) !=
                ESP_OK ||
            mbedtls_sha256_update(&sha, buffer, count) != 0) {
            readOk = false;
            break;
        }
        offset += count;
    }
    uint8_t digest[32]{};
    const bool hashOk = readOk &&
                        mbedtls_sha256_finish(&sha, digest) == 0 &&
                        memcmp(digest, imageSha256_, sizeof(digest)) == 0;
    mbedtls_sha256_free(&sha);
    if (!hashOk) {
        error = HashMismatch;
        return false;
    }
    if (!verifySignature(manifestWire_, sizeof(manifestWire_))) {
        error = SignatureRequired;
        return false;
    }

    lastOtaError_ = esp_ota_end(otaHandle_);
    otaHandle_ = 0;
    if (lastOtaError_ != ESP_OK) {
        error = BadImage;
        return false;
    }
    if (!verifyEmbeddedIdentity(targetPartition_, imageSize_, manifest_)) {
        error = BadImage;
        return false;
    }
    state_ = State::Prepared;
    lastError_ = Ok;
    error = Ok;
    return true;
}

bool OtaReceiver::handleActivate(Error &error) {
    if (state_ != State::Prepared || targetPartition_ == nullptr) {
        error = BadState;
        return false;
    }
#if !defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE) || \
    !CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    error = RollbackUnavailable;
    return false;
#else
    lastOtaError_ = esp_ota_set_boot_partition(targetPartition_);
    if (lastOtaError_ != ESP_OK ||
        esp_ota_get_boot_partition() != targetPartition_) {
        error = Flash;
        return false;
    }
    error = Ok;
    return true;
#endif
}

bool OtaReceiver::handleConfirm(Error &error) {
    if (state_ != State::PendingConfirm) {
        error = BadState;
        return false;
    }
    if (!localHealthPassed() || !healthPassed() || !peerCompatible()) {
        error = BadHardware;
        return false;
    }
#if !defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE) || \
    !CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    error = RollbackUnavailable;
    return false;
#else
    lastOtaError_ = esp_ota_mark_app_valid_cancel_rollback();
    if (lastOtaError_ != ESP_OK) {
        error = Flash;
        return false;
    }
    state_ = State::Idle;
    pendingConfirmStartedMs_ = 0;
    lastError_ = Ok;
    error = Ok;
    return true;
#endif
}

void OtaReceiver::handleFrame(const makcm::ota::Frame &frame) {
    using namespace makcm::ota;
    const bool validFlags = validFrameFlags(frame.command, frame.flags);
    if (validFlags && requestMatchesCache(frame)) {
        lastActivityMs_ = millis();
        (void)writeWire(responseCacheWire_, responseCacheLength_);
        return;
    }

    Error error = Ok;
    bool accepted = false;
    bool restartAfterResponse = false;
    bool releaseMaintenanceAfterResponse = false;
    bool rollbackAfterResponse = false;
    bool abortTransferOnFailure = false;

    if (frame.target != Target::Right) {
        error = BadTarget;
    } else if ((frame.flags & kFlagResponse) != 0 ||
               !validFlags) {
        error = BadFrame;
    } else {
        switch (frame.command) {
        case Command::Hello:
            if (frame.length == 0) accepted = true;
            else error = BadFrame;
            break;
        case Command::EnterMaintenance:
            if (frame.length != 0) {
                error = BadFrame;
                break;
            }
            accepted = enterMaintenance(frame.transaction_id, peerGeneration_);
            if (!accepted) error = BadState;
            break;
        case Command::Manifest:
            abortTransferOnFailure =
                (state_ == State::Receiving || state_ == State::Prepared) &&
                frame.transaction_id == transactionId_;
            if (!isMaintenanceActive() || state_ != State::Idle ||
                frame.length != kManifestWireSize || frame.offset != 0) {
                error = BadState;
                break;
            }
            if (frame.transaction_id != sessionTransactionId_) {
                error = BadTransaction;
                break;
            }
            if (!validateManifest(frame.payload, frame.length, error)) break;
            transactionId_ = frame.transaction_id;
            bytesWritten_ = 0;
            lastOtaError_ =
                esp_ota_begin(targetPartition_, imageSize_, &otaHandle_);
            if (lastOtaError_ != ESP_OK) {
                otaHandle_ = 0;
                error = Flash;
                break;
            }
            state_ = State::Receiving;
            accepted = true;
            break;
        case Command::Data:
            if (frame.transaction_id != transactionId_) {
                error = BadTransaction;
                break;
            }
            abortTransferOnFailure = true;
            accepted = handleData(frame.offset, frame.payload, frame.length,
                                  error);
            break;
        case Command::Prepare:
            if (frame.transaction_id != transactionId_) {
                error = BadTransaction;
                break;
            }
            abortTransferOnFailure = true;
            if (frame.length != 0) {
                error = BadFrame;
                break;
            }
            accepted = prepareImage(error);
            break;
        case Command::Status:
            if (!isMaintenanceActive()) {
                error = BadState;
                break;
            }
            if (fillStatusV2(frame, responseFrame_)) {
                (void)emitResponse(frame, responseFrame_, true);
                return;
            }
            error = Unsupported;
            break;
        case Command::Activate:
            if (frame.transaction_id != transactionId_) {
                error = BadTransaction;
                break;
            }
            abortTransferOnFailure = true;
            if (frame.length != 0) {
                error = BadFrame;
                break;
            }
            accepted = handleActivate(error);
            restartAfterResponse = accepted;
            break;
        case Command::Abort: {
            const bool transient =
                state_ == State::Receiving || state_ == State::Prepared;
            const uint32_t expectedTransactionId =
                transient ? transactionId_ : sessionTransactionId_;
            if (frame.offset != 0 || frame.length != 0 ||
                !isMaintenanceActive() ||
                frame.transaction_id != expectedTransactionId) {
                error = frame.transaction_id != expectedTransactionId
                            ? BadTransaction
                            : BadFrame;
                break;
            }
            if (state_ == State::PendingConfirm) {
#if defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE) && \
    CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
                accepted = true;
                rollbackAfterResponse = true;
#else
                error = RollbackUnavailable;
#endif
                break;
            }
            accepted = abortUpdate(Ok);
            if (!accepted) error = Flash;
            releaseMaintenanceAfterResponse = accepted;
            break;
        }
        case Command::ExitMaintenance:
            if (!isMaintenanceActive() || frame.offset != 0 ||
                frame.length != 0 || frame.transaction_id == 0) {
                error = frame.transaction_id == 0 ? BadTransaction : BadFrame;
                break;
            }
            accepted = true;
            releaseMaintenanceAfterResponse = true;
            break;
        case Command::Confirm:
            if (frame.length != 0 || !isMaintenanceActive() ||
                frame.transaction_id != sessionTransactionId_) {
                error = frame.transaction_id != sessionTransactionId_
                            ? BadTransaction
                            : BadFrame;
                break;
            }
            accepted = handleConfirm(error);
            releaseMaintenanceAfterResponse = accepted;
            break;
        default:
            error = Unsupported;
            break;
        }
    }

    if (!accepted && abortTransferOnFailure &&
        (state_ == State::Receiving || state_ == State::Prepared)) {
        const bool aborted = abortUpdate(error);
        if (!aborted) error = Flash;
    }
    lastError_ = accepted ? Ok : error;
    if (accepted &&
        (frame.command == Command::Manifest || frame.command == Command::Data ||
         frame.command == Command::Prepare ||
         frame.command == Command::Activate)) {
        lastActivityMs_ = millis();
    }
    fillResponse(frame, accepted ? Command::Ack : Command::Nack,
                 toNack(error), responseFrame_);
    (void)emitResponse(frame, responseFrame_, true);

    if (releaseMaintenanceAfterResponse) releaseMaintenance();
    if (restartAfterResponse) {
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
    }
    if (rollbackAfterResponse) {
        vTaskDelay(pdMS_TO_TICKS(100));
        pendingConfirmStartedMs_ = 0;
        lastOtaError_ = esp_ota_mark_app_invalid_rollback_and_reboot();
        lastError_ = Flash;
    }
}

void OtaReceiver::feed(uint8_t byte) {
    if (!isMaintenanceActive()) return;
    const makcm::ota::FeedResult result = decoder_.feed(byte, requestFrame_);
    if (result == makcm::ota::FeedResult::FrameReady) {
        handleFrame(requestFrame_);
    } else if (result == makcm::ota::FeedResult::FrameError) {
        lastError_ = BadFrame;
    }
}

void OtaReceiver::tick() {
    const uint32_t now = millis();
    if (state_ == State::PendingConfirm &&
        static_cast<uint32_t>(now - pendingConfirmStartedMs_) >
            kConfirmTimeoutMs) {
#if defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE) && \
    CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
        lastError_ = Timeout;
        pendingConfirmStartedMs_ = 0;
        lastOtaError_ = esp_ota_mark_app_invalid_rollback_and_reboot();
#else
        lastError_ = RollbackUnavailable;
#endif
        return;
    }
    const uint32_t elapsed =
        static_cast<uint32_t>(now - lastActivityMs_);
    if ((state_ == State::Receiving || state_ == State::Prepared) &&
        elapsed > kIdleTimeoutMs) {
        (void)abortUpdate(Timeout);
        releaseMaintenance();
        lastActivityMs_ = millis();
    }
}
