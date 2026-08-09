#include "ota_update_manager.h"

#include "OtaBoardConfig.h"
#include "OtaGeneration.h"

#include <cstdio>
#include <cstring>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_log.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/ecp.h"
#include "mbedtls/private_access.h"

namespace makcm::native {
namespace {
constexpr char kTag[] = "makcm-ota";

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

bool isTransient(makcm::ota::State state) {
    return state == makcm::ota::State::Receiving ||
           state == makcm::ota::State::Prepared;
}

bool isTransferMutation(makcm::ota::Command command) {
    using makcm::ota::Command;
    return command == Command::Manifest || command == Command::Data ||
           command == Command::Prepare || command == Command::Activate;
}

bool isCacheable(makcm::ota::Command command) {
    return command != makcm::ota::Command::Hello &&
           command != makcm::ota::Command::Status;
}
} // namespace

bool OtaTrustAnchor::configured() const {
    return !allZero(key_id, sizeof(key_id)) &&
           !allZero(public_x, sizeof(public_x)) &&
           !allZero(public_y, sizeof(public_y));
}

OtaUpdateManager::~OtaUpdateManager() {
    (void)resetDownload();
    mbedtls_sha256_free(&image_hash_);
}

bool OtaUpdateManager::begin(const OtaUpdateConfig &config, uint32_t now_ms) {
    const volatile uint8_t *embedded_identity =
        reinterpret_cast<const volatile uint8_t *>(
            &g_makcm_left_image_identity);
    if (embedded_identity[0] != static_cast<uint8_t>('M') ||
        embedded_identity[sizeof(g_makcm_left_image_identity) - 1] != 0 ||
        !makcm::ota::validEmbeddedImageIdentity(g_makcm_left_image_identity))
        return false;
    if ((config.role != makcm::ota::Target::Left &&
         config.role != makcm::ota::Target::Right) ||
        config.protocol_version == 0 || config.current_generation == 0 ||
        config.peer_generation == 0 || config.partition_schema == 0 ||
        config.hardware_revision == nullptr || config.project_name == nullptr ||
        config.quiesce == nullptr || config.local_health_check == nullptr ||
        config.peer_health_check == nullptr ||
        config.maintenance_timeout_ms < 1000 ||
        config.confirm_timeout_ms < 1000)
        return false;

    config_ = config;
    mbedtls_sha256_init(&image_hash_);
    boot_evidence_.reset_reason = esp_reset_reason();
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
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

    running_image_metadata_valid_ = false;
    if (running != nullptr &&
        esp_partition_get_sha256(running, running_image_sha256_) == ESP_OK) {
        esp_app_desc_t descriptor{};
        if (esp_ota_get_partition_description(running, &descriptor) == ESP_OK) {
            std::memcpy(running_app_version_, descriptor.version,
                        sizeof(running_app_version_));
            running_app_version_[sizeof(running_app_version_) - 1] = '\0';
            running_image_metadata_valid_ = true;
        }
    }

    const bool pending = boot_evidence_.pre_state_error == ESP_OK &&
                         image_state == ESP_OTA_IMG_PENDING_VERIFY;
    state_.store(pending ? makcm::ota::State::PendingConfirm
                         : makcm::ota::State::Idle,
                 std::memory_order_release);
    maintenance_exclusive_.store(pending, std::memory_order_release);
    local_boot_validated_ = boot_evidence_.pre_state_error == ESP_OK &&
                            image_state == ESP_OTA_IMG_VALID;
    pending_confirm_started_ms_ = pending ? now_ms : 0;
    last_activity_ms_ = now_ms;
    return true;
}

bool OtaUpdateManager::takeRestartRequest() {
    if (rollback_requested_.exchange(false, std::memory_order_acq_rel)) {
#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
        boot_evidence_.last_ota_error =
            esp_ota_mark_app_invalid_rollback_and_reboot();
        ESP_LOGE(kTag, "rollback-and-reboot returned: %s (%d)",
                 esp_err_to_name(boot_evidence_.last_ota_error),
                 static_cast<int>(boot_evidence_.last_ota_error));
#else
        boot_evidence_.last_ota_error = ESP_ERR_NOT_SUPPORTED;
#endif
        return false;
    }
    return restart_requested_.exchange(false, std::memory_order_acq_rel);
}

uint32_t OtaUpdateManager::statusFlags() const {
    uint32_t flags = 0;
#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    flags |= makcm::ota::kStatusRollbackSupported;
#endif
    if (config_.trust.configured()) flags |= makcm::ota::kStatusTrustConfigured;
    const bool local_healthy = config_.local_health_check != nullptr &&
                               config_.local_health_check(config_.context);
    const bool peer_healthy = config_.peer_health_check != nullptr &&
                              config_.peer_health_check(config_.context);
    const bool peer_compatible = !manifest_valid_ ||
        makcm::ota::manifestCompatibleWithPeer(manifest_,
                                                config_.peer_generation);
    if (peer_healthy && peer_compatible)
        flags |= makcm::ota::kStatusPeerCompatible;
    if (local_healthy) flags |= makcm::ota::kStatusHealthPassed;
    if (local_boot_validated_)
        flags |= makcm::ota::kStatusLocalBootValidated;
    if (maintenanceExclusive())
        flags |= makcm::ota::kStatusMaintenanceExclusive;
    return flags;
}

void OtaUpdateManager::fillResponse(
    const makcm::ota::Frame &request, makcm::ota::Command response_command,
    makcm::ota::NackReason reason, makcm::ota::Frame &response) const {
    std::memset(&response, 0, sizeof(response));
    response.target = makcm::ota::Target::Host;
    response.command = response_command;
    response.flags = makcm::ota::kFlagResponse;
    response.transaction_id = request.transaction_id;
    response.offset = isTransient(state_.load()) ? next_offset_ : 0;
    response.sequence = request.sequence;
    response.length = makcm::ota::kCommandResponsePayloadSize;
    makcm::ota::CommandResponsePayload result{};
    result.state = state_.load();
    result.last_command = request.command;
    result.reason = reason;
    result.next_offset = response.offset;
    result.target_generation = isTransient(state_.load())
        ? manifest_.generation : 0;
    result.active_generation = config_.current_generation;
    result.status_flags = statusFlags();
    makcm::ota::encodeCommandResponsePayload(result, response.payload);
}

bool OtaUpdateManager::fillStatusV2(const makcm::ota::Frame &request,
                                    makcm::ota::Frame &response) {
    using namespace makcm::ota;
    StatusPayloadV2 status{};
    status.role = config_.role;
    status.state = state_.load();
    status.last_command = request.command;
    status.last_response_command = Command::StatusResponse;
    status.status_flags = statusFlags();
    status.running_generation = config_.current_generation;
    status.partition_schema = config_.partition_schema;
    status.reset_reason = static_cast<uint32_t>(boot_evidence_.reset_reason);
    status.last_ota_error = static_cast<int32_t>(boot_evidence_.last_ota_error);
    std::snprintf(status.hardware_revision, sizeof(status.hardware_revision),
                  "%s", config_.hardware_revision);
    std::snprintf(status.project_name, sizeof(status.project_name), "%s",
                  config_.project_name);

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    esp_ota_img_states_t image_state = ESP_OTA_IMG_UNDEFINED;
    const esp_err_t state_error = running == nullptr || boot == nullptr
        ? ESP_ERR_NOT_FOUND
        : esp_ota_get_state_partition(running, &image_state);
    status.last_state_query_error = static_cast<int32_t>(state_error);
    if (state_error == ESP_OK) {
        status.running_slot = static_cast<uint8_t>(running->subtype);
        status.boot_slot = static_cast<uint8_t>(boot->subtype);
        status.image_state = static_cast<uint8_t>(image_state);
    }
    if (!running_image_metadata_valid_) return false;
    std::memcpy(status.running_image_sha256, running_image_sha256_,
                sizeof(status.running_image_sha256));
    std::memcpy(status.app_version, running_app_version_,
                sizeof(status.app_version));

    if (isTransient(status.state)) {
        status.next_offset = next_offset_;
        status.transaction_id = transaction_id_;
        status.target_generation = manifest_.generation;
        status.image_size = manifest_.image_size;
        status.target_slot = update_partition_ == nullptr
            ? 0xFF : static_cast<uint8_t>(update_partition_->subtype);
        std::memcpy(status.target_image_sha256, manifest_.image_sha256, 32);
        std::memcpy(status.manifest_sha256, manifest_sha256_, 32);
    }

    std::memset(&response, 0, sizeof(response));
    response.target = Target::Host;
    response.command = Command::StatusResponse;
    response.flags = kFlagResponse | kFlagStatusV2;
    response.transaction_id = request.transaction_id;
    response.offset = isTransient(status.state) ? next_offset_ : 0;
    response.sequence = request.sequence;
    if (!encodeStatusV2(status, response.payload, sizeof(response.payload)))
        return false;
    response.length = kStatusV2PayloadSize;
    return true;
}

bool OtaUpdateManager::requestMatchesCache(
    const makcm::ota::Frame &request) const {
    if (!cache_valid_ ||
        (request.flags & makcm::ota::kFlagRetransmit) == 0 ||
        request.target != cached_target_ ||
        request.command != cached_command_ ||
        (request.flags & ~makcm::ota::kFlagRetransmit) != cached_flags_ ||
        request.transaction_id != cached_transaction_id_ ||
        request.offset != cached_offset_ || request.sequence != cached_sequence_ ||
        request.length != cached_length_)
        return false;
    uint8_t digest[32]{};
    return mbedtls_sha256(request.payload, request.length, digest, 0) == 0 &&
           std::memcmp(digest, cached_payload_sha256_, 32) == 0;
}

void OtaUpdateManager::cacheResponse(const makcm::ota::Frame &request,
                                     const makcm::ota::Frame &response) {
    if (!isCacheable(request.command) ||
        mbedtls_sha256(request.payload, request.length,
                       cached_payload_sha256_, 0) != 0) {
        clearResponseCache();
        return;
    }
    cached_target_ = request.target;
    cached_command_ = request.command;
    cached_flags_ = request.flags & ~makcm::ota::kFlagRetransmit;
    cached_transaction_id_ = request.transaction_id;
    cached_offset_ = request.offset;
    cached_sequence_ = request.sequence;
    cached_length_ = request.length;
    cached_response_ = response;
    cache_valid_ = true;
}

void OtaUpdateManager::clearResponseCache() {
    cache_valid_ = false;
    std::memset(cached_payload_sha256_, 0, sizeof(cached_payload_sha256_));
}

bool OtaUpdateManager::handle(const makcm::ota::Frame &request,
                              makcm::ota::Frame &response, uint32_t now_ms) {
    using makcm::ota::Command;
    using makcm::ota::NackReason;
    const bool valid_flags =
        makcm::ota::validFrameFlags(request.command, request.flags);
    if (valid_flags && requestMatchesCache(request)) {
        response = cached_response_;
        last_activity_ms_ = now_ms;
        return response.command == Command::Ack;
    }

    NackReason reason = NackReason::None;
    bool ok = false;
    if (request.target != config_.role) {
        reason = NackReason::WrongTarget;
    } else if ((request.flags & makcm::ota::kFlagResponse) != 0) {
        reason = NackReason::Malformed;
    } else if (!valid_flags) {
        reason = NackReason::Malformed;
    } else if (request.command == Command::Hello && request.length == 0) {
        ok = true;
    } else if (request.command == Command::Status) {
        if (request.length != 0 ||
            (request.flags & ~makcm::ota::kFlagRetransmit) !=
                makcm::ota::kFlagStatusV2)
            reason = NackReason::Malformed;
        else
            ok = true;
    } else if (request.command == Command::EnterMaintenance) {
        ok = enter(request, now_ms, reason);
    } else if (request.command == Command::ExitMaintenance) {
        reason = NackReason::InvalidState;
    } else if (request.command == Command::Abort) {
        if (request.transaction_id == 0 || request.offset != 0 ||
            request.length != 0) {
            reason = NackReason::Malformed;
        } else if (isTransient(state_.load()) &&
                   request.transaction_id != transaction_id_) {
            reason = NackReason::WrongTransaction;
        } else {
            ok = abort(now_ms, reason);
        }
    } else if (request.command == Command::Confirm &&
               state_.load() == makcm::ota::State::PendingConfirm) {
        if (request.transaction_id == 0 || request.offset != 0 ||
            request.length != 0)
            reason = NackReason::Malformed;
        else
            ok = confirm(now_ms, reason);
    } else if (request.transaction_id == 0 ||
               request.transaction_id != transaction_id_) {
        reason = NackReason::WrongTransaction;
    } else {
        switch (request.command) {
        case Command::Manifest:
            ok = acceptManifest(request, now_ms, reason);
            break;
        case Command::Data:
            ok = acceptData(request, now_ms, reason);
            break;
        case Command::Prepare:
            ok = prepare(request, now_ms, reason);
            break;
        case Command::Activate:
            ok = activate(request, now_ms, reason);
            break;
        default:
            reason = NackReason::InvalidState;
            break;
        }
    }

    if (!ok && valid_flags && request.target == config_.role &&
        request.transaction_id == transaction_id_ &&
        isTransferMutation(request.command) && isTransient(state_.load()))
        clearTransfer(now_ms);
    if (request.command == Command::Status && ok) {
        if (!fillStatusV2(request, response)) {
            ok = false;
            reason = NackReason::ImageVerificationFailed;
            fillResponse(request, Command::Nack, reason, response);
        }
    } else {
        fillResponse(request, ok ? Command::Ack : Command::Nack,
                     reason, response);
    }
    if (isCacheable(request.command)) cacheResponse(request, response);
    return ok;
}

bool OtaUpdateManager::enter(const makcm::ota::Frame &request, uint32_t now_ms,
                             makcm::ota::NackReason &reason) {
    if (request.transaction_id == 0 || request.offset != 0 ||
        request.length != 0) {
        reason = makcm::ota::NackReason::Malformed;
        return false;
    }
    if (state_.load() != makcm::ota::State::Idle) {
        reason = makcm::ota::NackReason::InvalidState;
        return false;
    }
    if (maintenanceExclusive()) {
        if (transaction_id_ != request.transaction_id) {
            reason = makcm::ota::NackReason::Busy;
            return false;
        }
        last_activity_ms_ = now_ms;
        return true;
    }
    if (!config_.quiesce(config_.context)) {
        reason = makcm::ota::NackReason::Busy;
        return false;
    }
    clearResponseCache();
    transaction_id_ = request.transaction_id;
    maintenance_exclusive_.store(true, std::memory_order_release);
    last_activity_ms_ = now_ms;
    return true;
}

bool OtaUpdateManager::verifyManifestSignature(const uint8_t *wire) const {
    if (wire == nullptr || !config_.trust.configured() ||
        std::memcmp(wire + 112, config_.trust.key_id,
                    sizeof(config_.trust.key_id)) != 0)
        return false;
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
    if (!maintenanceExclusive() || state_.load() != makcm::ota::State::Idle ||
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

    const esp_partition_t *candidate =
        esp_ota_get_next_update_partition(nullptr);
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_app_desc_t running_descriptor{};
    if (candidate == nullptr || candidate == running ||
        candidate->type != ESP_PARTITION_TYPE_APP ||
        manifest.image_size > candidate->size || running == nullptr ||
        esp_ota_get_partition_description(running, &running_descriptor) != ESP_OK ||
        !exactField(config_.project_name, running_descriptor.project_name,
                    sizeof(running_descriptor.project_name)) ||
        !exactGenerationVersion(config_.current_generation,
                                running_descriptor.version,
                                sizeof(running_descriptor.version))) {
        reason = makcm::ota::NackReason::ManifestRejected;
        return false;
    }

    esp_ota_handle_t handle = 0;
    boot_evidence_.last_ota_error =
        esp_ota_begin(candidate, manifest.image_size, &handle);
    if (boot_evidence_.last_ota_error != ESP_OK) {
        reason = makcm::ota::NackReason::FlashWriteFailed;
        return false;
    }
    if (mbedtls_sha256_starts(&image_hash_, 0) != 0) {
        (void)esp_ota_abort(handle);
        reason = makcm::ota::NackReason::ImageVerificationFailed;
        return false;
    }

    manifest_ = manifest;
    std::memcpy(manifest_wire_, request.payload, sizeof(manifest_wire_));
    if (mbedtls_sha256(manifest_wire_, sizeof(manifest_wire_),
                       manifest_sha256_, 0) != 0) {
        (void)esp_ota_abort(handle);
        manifest_ = {};
        std::memset(manifest_wire_, 0, sizeof(manifest_wire_));
        reason = makcm::ota::NackReason::ImageVerificationFailed;
        return false;
    }
    update_partition_ = candidate;
    ota_handle_ = handle;
    ota_open_ = true;
    hash_open_ = true;
    manifest_valid_ = true;
    next_offset_ = 0;
    state_.store(makcm::ota::State::Receiving, std::memory_order_release);
    last_activity_ms_ = now_ms;
    return true;
}

bool OtaUpdateManager::validateFirstChunk(const uint8_t *data,
                                          size_t length) const {
    const size_t descriptor_offset = sizeof(esp_image_header_t) +
                                     sizeof(esp_image_segment_header_t);
    if (data == nullptr || length < descriptor_offset + sizeof(esp_app_desc_t) ||
        data[0] != ESP_IMAGE_HEADER_MAGIC)
        return false;
    esp_app_desc_t descriptor{};
    std::memcpy(&descriptor, data + descriptor_offset, sizeof(descriptor));
    return descriptor.magic_word == ESP_APP_DESC_MAGIC_WORD &&
           exactField(config_.project_name, descriptor.project_name,
                      sizeof(descriptor.project_name));
}

bool OtaUpdateManager::validateEmbeddedIdentity(
    const esp_partition_t *partition, uint32_t image_size,
    uint32_t generation) const {
    if (partition == nullptr ||
        image_size < sizeof(makcm::ota::EmbeddedImageIdentity))
        return false;
    const void *mapped = nullptr;
    esp_partition_mmap_handle_t handle = 0;
    if (esp_partition_mmap(partition, 0, image_size,
                           ESP_PARTITION_MMAP_DATA, &mapped, &handle) != ESP_OK ||
        mapped == nullptr)
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
            exactField(config_.hardware_revision, identity.hardware_revision,
                       sizeof(identity.hardware_revision)) &&
            exactField(config_.project_name, identity.project_name,
                       sizeof(identity.project_name)) &&
            length > 0 && static_cast<size_t>(length) < sizeof(expected_version) &&
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
        request.length > manifest_.chunk_size || request.offset != next_offset_ ||
        static_cast<uint64_t>(request.offset) + request.length >
            manifest_.image_size) {
        reason = request.offset != next_offset_
            ? makcm::ota::NackReason::WrongOffset
            : makcm::ota::NackReason::InvalidState;
        return false;
    }
    if (next_offset_ == 0 &&
        !validateFirstChunk(request.payload, request.length)) {
        reason = makcm::ota::NackReason::ManifestRejected;
        return false;
    }
    boot_evidence_.last_ota_error =
        esp_ota_write(ota_handle_, request.payload, request.length);
    if (boot_evidence_.last_ota_error != ESP_OK ||
        mbedtls_sha256_update(&image_hash_, request.payload, request.length) != 0) {
        reason = makcm::ota::NackReason::FlashWriteFailed;
        return false;
    }
    next_offset_ += request.length;
    last_activity_ms_ = now_ms;
    return true;
}

bool OtaUpdateManager::prepare(const makcm::ota::Frame &request,
                               uint32_t now_ms,
                               makcm::ota::NackReason &reason) {
    if (request.length != 0 || state_.load() != makcm::ota::State::Receiving ||
        !ota_open_ || !hash_open_ || next_offset_ != manifest_.image_size) {
        reason = makcm::ota::NackReason::InvalidState;
        return false;
    }
    uint8_t digest[32]{};
    if (mbedtls_sha256_finish(&image_hash_, digest) != 0 ||
        std::memcmp(digest, manifest_.image_sha256, sizeof(digest)) != 0) {
        reason = makcm::ota::NackReason::ImageVerificationFailed;
        return false;
    }
    hash_open_ = false;
    boot_evidence_.last_ota_error = esp_ota_end(ota_handle_);
    ota_open_ = false;
    ota_handle_ = 0;
    esp_app_desc_t descriptor{};
    const esp_err_t descriptor_result = update_partition_ == nullptr
        ? ESP_ERR_NOT_FOUND
        : esp_ota_get_partition_description(update_partition_, &descriptor);
    if (boot_evidence_.last_ota_error != ESP_OK ||
        descriptor_result != ESP_OK ||
        !exactField(config_.project_name, descriptor.project_name,
                    sizeof(descriptor.project_name)) ||
        !exactGenerationVersion(manifest_.generation, descriptor.version,
                                sizeof(descriptor.version)) ||
        !validateEmbeddedIdentity(update_partition_, manifest_.image_size,
                                  manifest_.generation)) {
        reason = makcm::ota::NackReason::ImageVerificationFailed;
        return false;
    }
    state_.store(makcm::ota::State::Prepared, std::memory_order_release);
    last_activity_ms_ = now_ms;
    return true;
}

bool OtaUpdateManager::activate(const makcm::ota::Frame &request,
                                uint32_t now_ms,
                                makcm::ota::NackReason &reason) {
    if (request.length != 0 || state_.load() != makcm::ota::State::Prepared ||
        update_partition_ == nullptr) {
        reason = makcm::ota::NackReason::InvalidState;
        return false;
    }
#if !CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    reason = makcm::ota::NackReason::RollbackUnavailable;
    return false;
#else
    boot_evidence_.last_ota_error =
        esp_ota_set_boot_partition(update_partition_);
    if (boot_evidence_.last_ota_error != ESP_OK ||
        esp_ota_get_boot_partition() != update_partition_) {
        reason = makcm::ota::NackReason::FlashWriteFailed;
        return false;
    }
    restart_requested_.store(true, std::memory_order_release);
    last_activity_ms_ = now_ms;
    return true;
#endif
}

bool OtaUpdateManager::confirm(uint32_t now_ms,
                               makcm::ota::NackReason &reason) {
    if (state_.load() != makcm::ota::State::PendingConfirm ||
        !config_.local_health_check(config_.context) ||
        !config_.peer_health_check(config_.context)) {
        reason = makcm::ota::NackReason::CompatibilityRejected;
        return false;
    }
#if !CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    reason = makcm::ota::NackReason::RollbackUnavailable;
    return false;
#else
    boot_evidence_.mark_valid_error =
        esp_ota_mark_app_valid_cancel_rollback();
    if (boot_evidence_.mark_valid_error != ESP_OK) {
        reason = makcm::ota::NackReason::FlashWriteFailed;
        return false;
    }
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t post_state = ESP_OTA_IMG_UNDEFINED;
    boot_evidence_.post_state_error = running == nullptr
        ? ESP_ERR_NOT_FOUND
        : esp_ota_get_state_partition(running, &post_state);
    boot_evidence_.post_state = post_state;
    if (boot_evidence_.post_state_error != ESP_OK ||
        post_state != ESP_OTA_IMG_VALID) {
        reason = makcm::ota::NackReason::ImageVerificationFailed;
        return false;
    }
    local_boot_validated_ = true;
    state_.store(makcm::ota::State::Idle, std::memory_order_release);
    maintenance_exclusive_.store(false, std::memory_order_release);
    transaction_id_ = 0;
    pending_confirm_started_ms_ = 0;
    last_activity_ms_ = now_ms;
    return true;
#endif
}

bool OtaUpdateManager::abort(uint32_t now_ms,
                             makcm::ota::NackReason &reason) {
    if (state_.load() == makcm::ota::State::PendingConfirm) {
#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
        rollback_requested_.store(true, std::memory_order_release);
        last_activity_ms_ = now_ms;
        return true;
#else
        reason = makcm::ota::NackReason::RollbackUnavailable;
        return false;
#endif
    }
    if (!resetDownload()) {
        reason = makcm::ota::NackReason::FlashWriteFailed;
        return false;
    }
    state_.store(makcm::ota::State::Idle, std::memory_order_release);
    maintenance_exclusive_.store(false, std::memory_order_release);
    transaction_id_ = 0;
    last_activity_ms_ = now_ms;
    return true;
}

void OtaUpdateManager::clearTransfer(uint32_t now_ms) {
    (void)resetDownload();
    state_.store(makcm::ota::State::Idle, std::memory_order_release);
    last_activity_ms_ = now_ms;
}

bool OtaUpdateManager::resetDownload() {
    bool ok = true;
    if (ota_open_) {
        boot_evidence_.last_ota_error = esp_ota_abort(ota_handle_);
        ok = boot_evidence_.last_ota_error == ESP_OK;
    }
    ota_open_ = false;
    ota_handle_ = 0;
    update_partition_ = nullptr;
    manifest_valid_ = false;
    manifest_ = {};
    std::memset(manifest_wire_, 0, sizeof(manifest_wire_));
    std::memset(manifest_sha256_, 0, sizeof(manifest_sha256_));
    if (hash_open_) {
        mbedtls_sha256_free(&image_hash_);
        mbedtls_sha256_init(&image_hash_);
    }
    hash_open_ = false;
    next_offset_ = 0;
    return ok;
}

void OtaUpdateManager::poll(uint32_t now_ms) {
    const auto current = state_.load();
    if ((maintenanceExclusive() && current != makcm::ota::State::PendingConfirm &&
         now_ms - last_activity_ms_ > config_.maintenance_timeout_ms)) {
        (void)resetDownload();
        state_.store(makcm::ota::State::Idle, std::memory_order_release);
        maintenance_exclusive_.store(false, std::memory_order_release);
        transaction_id_ = 0;
        clearResponseCache();
    } else if (current == makcm::ota::State::PendingConfirm &&
               now_ms - pending_confirm_started_ms_ >
                   config_.confirm_timeout_ms) {
#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
        rollback_requested_.store(true, std::memory_order_release);
#endif
    }
}

} // namespace makcm::native
