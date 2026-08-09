#pragma once

#include <atomic>
#include <stddef.h>
#include <stdint.h>

#include "OtaImageIdentity.h"
#include "OtaManifest.h"
#include "OtaProtocol.h"
#include "OtaStatusV2.h"

#include "esp_ota_ops.h"
#include "esp_system.h"
#include "mbedtls/sha256.h"

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
    bool (*local_health_check)(void *context) = nullptr;
    bool (*peer_health_check)(void *context) = nullptr;
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
    bool maintenanceExclusive() const {
        return maintenance_exclusive_.load(std::memory_order_acquire);
    }
    bool takeRestartRequest();
    bool trustConfigured() const { return config_.trust.configured(); }
    bool localBootValidated() const { return local_boot_validated_; }
    const BootEvidence &bootEvidence() const { return boot_evidence_; }

private:
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
    bool confirm(uint32_t now_ms, makcm::ota::NackReason &reason);
    bool abort(uint32_t now_ms, makcm::ota::NackReason &reason);
    void clearTransfer(uint32_t now_ms);
    bool resetDownload();
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
    bool requestMatchesCache(const makcm::ota::Frame &request) const;
    void cacheResponse(const makcm::ota::Frame &request,
                       const makcm::ota::Frame &response);
    void clearResponseCache();

    OtaUpdateConfig config_{};
    std::atomic<makcm::ota::State> state_{makcm::ota::State::Idle};
    std::atomic<bool> maintenance_exclusive_{false};
    std::atomic<bool> restart_requested_{false};
    std::atomic<bool> rollback_requested_{false};
    const esp_partition_t *update_partition_ = nullptr;
    esp_ota_handle_t ota_handle_ = 0;
    bool ota_open_ = false;
    bool manifest_valid_ = false;
    bool hash_open_ = false;
    makcm::ota::Manifest manifest_{};
    uint8_t manifest_wire_[makcm::ota::kManifestWireSize]{};
    uint8_t manifest_sha256_[32]{};
    mbedtls_sha256_context image_hash_{};
    uint32_t transaction_id_ = 0;
    uint32_t next_offset_ = 0;
    uint32_t last_activity_ms_ = 0;
    uint32_t pending_confirm_started_ms_ = 0;
    bool local_boot_validated_ = false;
    bool running_image_metadata_valid_ = false;
    uint8_t running_image_sha256_[32]{};
    char running_app_version_[32]{};

    bool cache_valid_ = false;
    makcm::ota::Target cached_target_ = makcm::ota::Target::Host;
    makcm::ota::Command cached_command_ = makcm::ota::Command::Hello;
    uint8_t cached_flags_ = 0;
    uint32_t cached_transaction_id_ = 0;
    uint32_t cached_offset_ = 0;
    uint32_t cached_sequence_ = 0;
    uint16_t cached_length_ = 0;
    uint8_t cached_payload_sha256_[32]{};
    makcm::ota::Frame cached_response_{};
    BootEvidence boot_evidence_{};
};

} // namespace makcm::native
