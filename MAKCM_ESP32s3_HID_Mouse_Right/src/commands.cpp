#include "EspUsbHost.h"
#include "esp_log.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>


bool enableYield = false;

namespace {

// Seven distinct refusals used to share one bare OTA_UNAVAILABLE line, so the
// host could not tell "this board is busy with a device" from "this board can
// never accept a bootstrap again".  The line now carries its site and the
// hostState that produced it; the left bridge forwards both.
enum class BootstrapRefusal : uint8_t {
    UntrustedSource = 1,
    MalformedRequest = 2,
    PendingOtherTransaction = 3,
    RebindRejected = 4,
    ReservationLost = 5,
    MaintenanceRejected = 6,
    HostStateUnavailable = 7,
    BootstrapWaitTimeout = 8,
};

}  // namespace

void EspUsbHost::sendBootstrapRefusal(uint8_t site)
{
    // The third field is enterMaintenance's own site.  Sites 4 and 6 are just
    // "enterMaintenance said no", which has six causes of its own -- and this
    // line is the only channel that can report them, because a board that is
    // not in maintenance cannot answer Status V2 either.
    char line[64] = {};
    snprintf(line, sizeof(line), "OTA_UNAVAILABLE %u %u %u\n",
             static_cast<unsigned>(site),
             static_cast<unsigned>(hostState.load(std::memory_order_acquire)),
             static_cast<unsigned>(otaReceiver.lastMaintenanceReject()));
    otaReceiver.sendText(line);
}

void EspUsbHost::handleIncomingCommands(const String &command,
                                        bool trustedMaintenanceSource)
{
    ESP_LOGI("EspUsbHost", "Received command: %s", command.c_str());

    if (command.startsWith("OTA_MAINT_BEGIN ") || command == "OTA_ENTER")
    {
        if (!trustedMaintenanceSource) {
            // OTA bootstrap is accepted only from UART1, which is physically
            // connected to the trusted Linux-facing left board.  Serial0 is
            // a generic/debug command source and must never acquire firmware
            // update ownership.  Still answer on UART1 so a mis-routed probe
            // is observable instead of timing out.
            ESP_LOGW("EspUsbHost", "Rejected OTA bootstrap from untrusted command source");
            sendBootstrapRefusal(
                static_cast<uint8_t>(BootstrapRefusal::UntrustedSource));
            return;
        }
        // Bootstrap is text-only and deliberately separate from MKOT.  A
        // transaction id binds the following binary frames to this session.
        uint32_t transactionId = 0;
        uint32_t peerGeneration = 0;
        if (command.startsWith("OTA_MAINT_BEGIN ")) {
            unsigned long parsedTransaction = 0;
            unsigned long parsedPeer = 0;
            char tail = '\0';
            const int fields = sscanf(command.c_str() + 16, "%lu %lu%c",
                                      &parsedTransaction, &parsedPeer, &tail);
            if ((fields == 1 || fields == 2) && parsedTransaction != 0 &&
                parsedTransaction <= UINT32_MAX && parsedPeer <= UINT32_MAX) {
                transactionId = static_cast<uint32_t>(parsedTransaction);
                peerGeneration = static_cast<uint32_t>(parsedPeer);
            }
        }
        if (transactionId == 0) {
            // This path is intentionally text-only and fail-closed.  Windows
            // USB1 never reaches it; only the Linux-side left bridge may send
            // the bootstrap line over UART1.
            sendBootstrapRefusal(
                static_cast<uint8_t>(BootstrapRefusal::MalformedRequest));
            return;
        }
        if (hostState.load(std::memory_order_acquire) == HostState::Fault) {
            // Fault means the USB *data plane* is untrustworthy: it is set on a
            // failed hot-plug cleanup and cleared nowhere, so a power cycle was
            // the only exit.  Refusing OTA here was backwards -- OTA's first
            // act is to tear USB down entirely, so it is the one operation that
            // does not depend on the thing that broke, and it is the recovery
            // path being blocked by the failure it would repair.
            //
            // A failed cleanup is also exactly the case where an in-place
            // teardown cannot be trusted, so take the teardown that always
            // works.  OTA_PENDING is already the bridge's "keep retrying"
            // marker, so the host's existing retry loop rides through the
            // reboot and the next attempt starts from a clean Empty state.
            ESP_LOGW("EspUsbHost",
                     "OTA bootstrap while fail-closed; restarting to recover");
            otaReceiver.sendText("OTA_PENDING\n");
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
        }

        if (otaBootstrapPending) {
            // OtaClient retries the same bootstrap request while the USB
            // client task finishes teardown.  Keep the pending transaction
            // idempotent and do not restart its cleanup deadline.
            if (transactionId == otaBootstrapTransaction &&
                peerGeneration == otaBootstrapPeerGeneration) {
                otaReceiver.sendText("OTA_PENDING\n");
            } else {
                sendBootstrapRefusal(
                    static_cast<uint8_t>(BootstrapRefusal::PendingOtherTransaction));
            }
            return;
        }

        // NVS can relight maintenanceActive_/otaHostReserved on every boot.
        // That must not make OTA_MAINT_BEGIN look like a dead UART: try to
        // re-bind the bootstrap transaction (or explicitly refuse) instead of
        // falling into the reserved-CAS UNAVAILABLE path with no recovery.
        if (otaReceiver.isMaintenanceActive() &&
            otaHostReserved.load(std::memory_order_acquire)) {
            if (otaReceiver.enterMaintenance(transactionId, peerGeneration)) {
                deviceMouseReady = false;
                char ready[48] = {};
                snprintf(ready, sizeof(ready), "OTA_READY %lu\n",
                         static_cast<unsigned long>(transactionId));
                otaReceiver.sendText(ready);
            } else {
                sendBootstrapRefusal(
                    static_cast<uint8_t>(BootstrapRefusal::RebindRejected));
            }
            return;
        }

        bool unreserved = false;
        if (!otaHostReserved.compare_exchange_strong(
                unreserved, true, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            sendBootstrapRefusal(
                static_cast<uint8_t>(BootstrapRefusal::ReservationLost));
            return;
        }

        HostState observed = HostState::Empty;
        if (hostState.compare_exchange_strong(
                observed, HostState::OtaReserved,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            // Empty->OtaReserved and Empty->Opening use the same CAS. Exactly
            // one owner wins, closing the hot-plug/bootstrap TOCTOU.
            if (otaReceiver.enterMaintenance(transactionId, peerGeneration)) {
                deviceMouseReady = false;
                char ready[48] = {};
                snprintf(ready, sizeof(ready), "OTA_READY %lu\n",
                         static_cast<unsigned long>(transactionId));
                otaReceiver.sendText(ready);
            } else {
                hostState.store(HostState::Empty, std::memory_order_release);
                otaHostReserved.store(false, std::memory_order_release);
                sendBootstrapRefusal(
                    static_cast<uint8_t>(BootstrapRefusal::MaintenanceRejected));
            }
            return;
        }

        if (observed == HostState::Active ||
            observed == HostState::Opening ||
            observed == HostState::Cleaning) {
            deviceMouseReady = false;
            if (cleanupComplete != nullptr) xSemaphoreTake(cleanupComplete, 0);
            hostState = HostState::Cleaning;
            cleanupRequested = true;
            otaBootstrapPending = true;
            otaBootstrapTransaction = transactionId;
            otaBootstrapPeerGeneration = peerGeneration;
            otaBootstrapStartedMs = millis();
            // This marker is consumed by the left bridge only.  It keeps the
            // bridge pending across OtaClient retries without exposing a
            // maintenance entry point on the Windows-facing USB1 path.
            otaReceiver.sendText("OTA_PENDING\n");
            return;
        }
        otaHostReserved.store(false, std::memory_order_release);
        sendBootstrapRefusal(
            static_cast<uint8_t>(BootstrapRefusal::HostStateUnavailable));
    }
    else if (command == "DEBUG_ON")
    {
        debugModeActive = true;
        serial1Send("Debug mode activated.\n");
        serial1Send("USB_ISDEBUG\n");
        ESP_LOGI("EspUsbHost", "Debug mode activated.");
    }
    else if (command == "DEBUG_OFF")
    {
        if (USB_IS_DEBUG)
        {
            return;
        }
        else
        {
            debugModeActive = false;
            ESP_LOGI("EspUsbHost", "Debug mode deactivated. System will restart.");
            serial1Send("USB_GOODBYE\n");
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
        }
    }
    else if (command == "DEBUG_STATUS")
    {
        sendRuntimeStatus();
    }
    else if (command == "READY")
    {
        if (debugModeActive)
        {
            serial1Send("USB_ISDEBUG\n");
            ESP_LOGI("EspUsbHost", "Debug mode is active. Sent USB_ISDEBUG.");
        }
        else
        {
            if (EspUsbHost::deviceConnected)
            {
                serial1Send("USB_HELLO\n");
                ESP_LOGI("EspUsbHost", "Device is connected.");
            }
            else
            {
                serial1Send("USB_ISNULL\n");
                ESP_LOGW("EspUsbHost", "No device is connected.");
            }
        }
    }
    else if (command == "USB_INIT")
    {
        // Protocol v2 requires a CRC-protected ProfileAccepted frame carrying
        // the current session ID. A session-less text command must never open
        // the input gate for a newly attached device.
        serial1Send("USB_INIT_DEPRECATED\n");
        ESP_LOGW("EspUsbHost", "Ignored session-less USB_INIT command.");
    }
    else if (command == "sendDeviceInfo")
    {
        sendDeviceInfo();
        serial1Send("Device information sent.\n");
        ESP_LOGI("EspUsbHost", "Sending device information.");
    }
    else if (command == "sendDescriptorDevice")
    {
        sendDescriptorDevice();
        serial1Send("Device descriptor sent.\n");
        ESP_LOGI("EspUsbHost", "Sending device descriptor.");
    }
    else if (command == "sendEndpointDescriptors")
    {
        sendEndpointDescriptors();
        serial1Send("Endpoint descriptors sent.\n");
        ESP_LOGI("EspUsbHost", "Sending endpoint descriptors.");
    }
    else if (command == "sendInterfaceDescriptors")
    {
        sendInterfaceDescriptors();
        serial1Send("Interface descriptors sent.\n");
        ESP_LOGI("EspUsbHost", "Sending interface descriptors.");
    }
    else if (command == "sendHidDescriptors")
    {
        sendHidDescriptors();
        serial1Send("HID descriptors sent.\n");
        ESP_LOGI("EspUsbHost", "Sending HID descriptors.");
    }
    else if (command == "sendIADescriptors")
    {
        sendIADescriptors();
        serial1Send("Interface Association Descriptors sent.\n");
        ESP_LOGI("EspUsbHost", "Sending Interface Association Descriptors.");
    }
    else if (command == "sendEndpointData")
    {
        sendEndpointData();
        serial1Send("Endpoint data sent.\n");
        ESP_LOGI("EspUsbHost", "Sending endpoint data.");
    }
    else if (command == "sendUnknownDescriptors")
    {
        sendUnknownDescriptors();
        serial1Send("Unknown descriptors sent.\n");
        ESP_LOGI("EspUsbHost", "Sending unknown descriptors.");
    }
    else if (command == "sendDescriptorconfig")
    {
        sendDescriptorconfig();
        serial1Send("Configuration descriptor sent.\n");
        ESP_LOGI("EspUsbHost", "Sending configuration descriptor.");
    }
    else if (command == "sendPassthroughProfile")
    {
        sendPassthroughProfile();
        ESP_LOGI("EspUsbHost", "Sending passthrough profile.");
    }
    else if (command == "YIELD")
    {
        enableYield = true;
        serial1Send("Yield enabled.\n");
        ESP_LOGI("EspUsbHost", "Yield enabled.");
    }
    else if (command == "YIELD_END")
    {
        enableYield = false;
        serial1Send("Yield disabled.\n");
        ESP_LOGI("EspUsbHost", "Yield disabled.");
    }
    else
    {
        serial1Send("Unknown command received: %s\n", command.c_str());
        ESP_LOGW("EspUsbHost", "Unknown command received: %s", command.c_str());
    }
}

void EspUsbHost::serviceOtaBootstrap()
{
    if (!otaBootstrapPending) {
        // Release from the fact that no maintenance session exists, never from
        // otaHostReserved.  Two bootstrap failure paths -- the lost
        // Empty->OtaReserved CAS below, and the bootstrap-wait timeout -- clear
        // otaHostReserved while leaving hostState latched at OtaReserved.
        // Gating the repair on otaHostReserved meant precisely those paths
        // could never be repaired: hostState stayed OtaReserved forever, so
        // every later OTA_MAINT_BEGIN fell through to OTA_UNAVAILABLE and every
        // later controller hot-plug stayed queued behind a reservation nobody
        // held.  Only a power cycle cleared it.
        //
        // receiveSerial1 owns both this function and handleIncomingCommands, so
        // a bootstrap in progress cannot interleave with the release.
        if (!otaReceiver.isMaintenanceActive()) {
            HostState reserved = HostState::OtaReserved;
            (void)hostState.compare_exchange_strong(
                reserved, HostState::Empty,
                std::memory_order_acq_rel, std::memory_order_acquire);
            otaHostReserved.store(false, std::memory_order_release);
        }
        return;
    }

    const uint32_t transactionId = otaBootstrapTransaction;
    const uint32_t peerGeneration = otaBootstrapPeerGeneration;
    const bool timedOut = static_cast<uint32_t>(millis() -
                                                  otaBootstrapStartedMs) >=
                          kOtaBootstrapWaitMs;

    HostState expected = HostState::Empty;
    if (hostState.compare_exchange_strong(
            expected, HostState::OtaReserved,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        otaBootstrapPending = false;
        otaBootstrapTransaction = 0;
        otaBootstrapPeerGeneration = 0;
        otaBootstrapStartedMs = 0;
        if (otaReceiver.enterMaintenance(transactionId, peerGeneration)) {
            deviceMouseReady = false;
            char ready[48] = {};
            snprintf(ready, sizeof(ready), "OTA_READY %lu\n",
                     static_cast<unsigned long>(transactionId));
            otaReceiver.sendText(ready);
        } else {
            hostState.store(HostState::Empty, std::memory_order_release);
            otaHostReserved.store(false, std::memory_order_release);
            sendBootstrapRefusal(
                static_cast<uint8_t>(BootstrapRefusal::MaintenanceRejected));
        }
        return;
    }

    if (hostState.load(std::memory_order_acquire) == HostState::Fault) {
        // This is the cleanup we asked for, and it failed.  Same reasoning as
        // the bootstrap entry point: the teardown OTA needs cannot be trusted
        // in place, and a reset is the teardown that always works.  Clearing
        // the pending state first keeps the restart idempotent if the host's
        // retry lands before the reboot completes.
        otaBootstrapPending = false;
        otaBootstrapTransaction = 0;
        otaBootstrapPeerGeneration = 0;
        otaBootstrapStartedMs = 0;
        otaHostReserved.store(false, std::memory_order_release);
        ESP_LOGW("EspUsbHost",
                 "OTA teardown failed fail-closed; restarting to recover");
        otaReceiver.sendText("OTA_PENDING\n");
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
    }

    if (timedOut) {
        // Do not leave the left bridge waiting for a line that can never
        // arrive.  The host can retry after the explicit refusal.
        otaBootstrapPending = false;
        otaBootstrapTransaction = 0;
        otaBootstrapPeerGeneration = 0;
        otaBootstrapStartedMs = 0;
        otaHostReserved.store(false, std::memory_order_release);
        sendBootstrapRefusal(
            static_cast<uint8_t>(BootstrapRefusal::BootstrapWaitTimeout));
    }
}
