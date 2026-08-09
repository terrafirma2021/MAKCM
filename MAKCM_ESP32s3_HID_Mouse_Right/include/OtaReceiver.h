#pragma once

#include <Arduino.h>
#include <atomic>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stddef.h>
#include <stdint.h>

#include "OtaManifest.h"
#include "OtaProtocol.h"
#include "OtaStatusV2.h"

// Right-board OTA is intentionally volatile. The bootloader owns the only
// cross-reset fact: whether the running image is pending verification.
class OtaReceiver {
public:
    static constexpr uint32_t kIdleTimeoutMs = 45000;
    static constexpr uint32_t kConfirmTimeoutMs = 120000;
    static constexpr size_t kMaxFrameSize = makcm::ota::kMaxWireSize;
    static constexpr size_t kMaxResponseWireSize =
        makcm::ota::kHeaderSize + makcm::ota::kStatusV2PayloadSize +
        makcm::ota::kCrcSize;

    using State = makcm::ota::State;
    using LocalHealthCheck = bool (*)(void *context);
    using PeerHealthCheck = bool (*)(void *context);

    enum Error : uint16_t {
        Ok = 0,
        BadFrame,
        BadVersion,
        BadTarget,
        BadTransaction,
        BadState,
        BadManifest,
        BadRole,
        BadHardware,
        BadPartition,
        BadImage,
        BadOffset,
        BadCrc,
        Timeout,
        HashMismatch,
        SignatureRequired,
        RollbackUnavailable,
        Flash,
        Unsupported,
    };

    enum class MaintenanceReject : uint8_t {
        None = 0,
        NotInitialized,
        ZeroTransaction,
        SerialBusy,
    };

    void begin(HardwareSerial *serial, SemaphoreHandle_t *mutex,
               LocalHealthCheck localHealthCheck = nullptr,
               PeerHealthCheck peerHealthCheck = nullptr,
               void *healthContext = nullptr);
    bool enterMaintenance(uint32_t transactionId = 0,
                          uint32_t peerGeneration = 0);
    MaintenanceReject lastMaintenanceReject() const {
        return lastMaintenanceReject_;
    }
    bool isMaintenanceActive() const {
        return maintenanceActive_.load(std::memory_order_acquire);
    }
    size_t decoderBuffered() const { return decoder_.buffered(); }
    void feed(uint8_t byte);
    void tick();
    bool sendText(const char *text);
    State state() const { return state_; }
    bool peerCompatible() const;
    bool healthPassed() const {
        return peerHealthCheck_ != nullptr && peerHealthCheck_(healthContext_);
    }
    bool localHealthPassed() const {
        return localHealthCheck_ != nullptr &&
               localHealthCheck_(healthContext_);
    }

private:
    struct RequestCacheKey {
        makcm::ota::Target target = makcm::ota::Target::Host;
        makcm::ota::Command command = makcm::ota::Command::Hello;
        uint8_t flags = 0;
        uint32_t transactionId = 0;
        uint32_t offset = 0;
        uint32_t sequence = 0;
        uint16_t length = 0;
        uint8_t payloadSha256[32]{};
    };

    void handleFrame(const makcm::ota::Frame &frame);
    bool writeWire(const uint8_t *data, size_t length);
    void fillResponse(const makcm::ota::Frame &request,
                      makcm::ota::Command command,
                      makcm::ota::NackReason reason,
                      makcm::ota::Frame &response) const;
    bool fillStatusV2(const makcm::ota::Frame &request,
                      makcm::ota::Frame &response);
    bool emitResponse(const makcm::ota::Frame &request,
                      const makcm::ota::Frame &response, bool cache);
    bool requestMatchesCache(const makcm::ota::Frame &request) const;
    void cacheRequest(const makcm::ota::Frame &request);
    void clearResponseCache();
    void releaseMaintenance();
    void resetTransient();
    bool abortUpdate(Error error);
    bool validateManifest(const uint8_t *payload, uint16_t length,
                          Error &error);
    bool verifySignature(const uint8_t *manifest, uint16_t length) const;
    bool handleData(uint32_t offset, const uint8_t *payload, uint16_t length,
                    Error &error);
    bool prepareImage(Error &error);
    bool handleActivate(Error &error);
    bool handleConfirm(Error &error);

    HardwareSerial *serial_ = nullptr;
    SemaphoreHandle_t *serialMutex_ = nullptr;
    LocalHealthCheck localHealthCheck_ = nullptr;
    PeerHealthCheck peerHealthCheck_ = nullptr;
    void *healthContext_ = nullptr;
    std::atomic<bool> maintenanceActive_{false};
    bool initialized_ = false;
    State state_ = State::Idle;
    uint32_t sessionTransactionId_ = 0;
    uint32_t transactionId_ = 0;
    uint32_t peerGeneration_ = 0;
    uint32_t targetGeneration_ = 0;
    uint32_t minPeerGeneration_ = 0;
    uint32_t maxPeerGeneration_ = 0;
    uint32_t imageSize_ = 0;
    uint32_t bytesWritten_ = 0;
    uint32_t lastActivityMs_ = 0;
    uint32_t pendingConfirmStartedMs_ = 0;
    Error lastError_ = Ok;
    MaintenanceReject lastMaintenanceReject_ = MaintenanceReject::None;
    uint8_t targetSubtype_ = 0xFF;
    uint8_t imageSha256_[32]{};
    uint8_t manifestSha256_[32]{};
    esp_err_t lastOtaError_ = ESP_OK;
    esp_err_t lastStateQueryError_ = ESP_OK;
    esp_ota_img_states_t lastImageState_ = ESP_OTA_IMG_UNDEFINED;
    esp_reset_reason_t resetReason_ = ESP_RST_UNKNOWN;
    uint8_t runningImageSha256_[32]{};
    bool runningImageSha256Valid_ = false;
    esp_ota_handle_t otaHandle_ = 0;
    const esp_partition_t *targetPartition_ = nullptr;
    makcm::ota::Manifest manifest_{};
    uint8_t manifestWire_[makcm::ota::kManifestWireSize]{};

    makcm::ota::StreamDecoder decoder_;
    makcm::ota::Frame requestFrame_{};
    makcm::ota::Frame responseFrame_{};
    uint8_t wireBuffer_[kMaxFrameSize]{};
    uint8_t statusV2Wire_[makcm::ota::kStatusV2PayloadSize]{};
    RequestCacheKey requestCache_{};
    uint8_t responseCacheWire_[kMaxResponseWireSize]{};
    size_t responseCacheLength_ = 0;
    bool responseCacheValid_ = false;
};

extern OtaReceiver otaReceiver;
