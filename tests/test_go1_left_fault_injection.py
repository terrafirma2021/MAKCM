"""Source contracts for the left-board RAM-only four-state OTA manager.

Hardware rollback and power-loss behavior still requires device testing.  These
checks keep the host build honest about the deliberately small lifecycle and
prevent the retired NVS journal from creeping back into the left target.
"""

from __future__ import annotations

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
LEFT = ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF"
HEADER = LEFT / "components" / "makcm_core" / "include" / "ota_update_manager.h"
SOURCE = LEFT / "components" / "makcm_core" / "ota_update_manager.cpp"
APP = LEFT / "main" / "app_main.cpp"


class LeftOtaFourStateSourceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.header = HEADER.read_text(encoding="utf-8")
        cls.source = SOURCE.read_text(encoding="utf-8")
        cls.app = APP.read_text(encoding="utf-8")

    def test_manager_has_no_persistent_transaction_authority(self) -> None:
        retired = (
            "OtaSnapshotV2",
            "LocalRecord",
            "SnapshotCommitFault",
            "nvs_handle_t",
            "nvs_open",
            "nvs_set_blob",
            "nvs_commit",
            "ledger",
            "tombstone",
        )
        combined = self.header + self.source
        for token in retired:
            with self.subTest(token=token):
                self.assertNotIn(token, combined)

    def test_boot_state_comes_only_from_running_partition_pending_verify(self) -> None:
        begin = self.source[
            self.source.index("bool OtaUpdateManager::begin") :
            self.source.index("bool OtaUpdateManager::takeRestartRequest")
        ]
        self.assertIn("esp_ota_get_state_partition(running, &image_state)", begin)
        self.assertIn("image_state == ESP_OTA_IMG_PENDING_VERIFY", begin)
        self.assertIn("makcm::ota::State::PendingConfirm", begin)
        self.assertIn("makcm::ota::State::Idle", begin)
        self.assertNotIn("Receiving", begin)
        self.assertNotIn("Prepared", begin)

    def test_manifest_opens_ota_only_from_maintenance_idle(self) -> None:
        manifest = self.source[
            self.source.index("bool OtaUpdateManager::acceptManifest") :
            self.source.index("bool OtaUpdateManager::validateFirstChunk")
        ]
        self.assertIn("!maintenanceExclusive()", manifest)
        self.assertIn("state_.load() != makcm::ota::State::Idle", manifest)
        self.assertIn("esp_ota_begin(candidate, manifest.image_size", manifest)
        self.assertIn("State::Receiving", manifest)

    def test_prepare_verifies_hash_end_descriptor_and_embedded_identity(self) -> None:
        prepare = self.source[
            self.source.index("bool OtaUpdateManager::prepare") :
            self.source.index("bool OtaUpdateManager::activate")
        ]
        for token in (
            "mbedtls_sha256_finish",
            "manifest_.image_sha256",
            "esp_ota_end",
            "esp_ota_get_partition_description",
            "validateEmbeddedIdentity",
            "State::Prepared",
        ):
            self.assertIn(token, prepare)

    def test_activate_acks_before_deferred_restart(self) -> None:
        activate = self.source[
            self.source.index("bool OtaUpdateManager::activate") :
            self.source.index("bool OtaUpdateManager::confirm")
        ]
        self.assertIn("esp_ota_set_boot_partition", activate)
        self.assertIn("restart_requested_.store(true", activate)
        self.assertNotIn("esp_restart", activate)
        self.assertIn("takeRestartRequest()", self.app)

    def test_confirm_requires_both_health_checks_and_marks_valid(self) -> None:
        confirm = self.source[
            self.source.index("bool OtaUpdateManager::confirm") :
            self.source.index("bool OtaUpdateManager::abort")
        ]
        self.assertIn("local_health_check", confirm)
        self.assertIn("peer_health_check", confirm)
        self.assertIn("esp_ota_mark_app_valid_cancel_rollback", confirm)
        self.assertIn("State::Idle", confirm)
        self.assertNotIn("pair_health_check", self.header + self.app)

    def test_abort_and_confirm_timeout_schedule_official_rollback(self) -> None:
        self.assertGreaterEqual(self.source.count("rollback_requested_.store(true"), 2)
        take = self.source[
            self.source.index("bool OtaUpdateManager::takeRestartRequest") :
            self.source.index("uint32_t OtaUpdateManager::statusFlags")
        ]
        self.assertIn("esp_ota_mark_app_invalid_rollback_and_reboot", take)

    def test_status_v2_exposes_transient_fields_only_in_transient_states(self) -> None:
        status = self.source[
            self.source.index("bool OtaUpdateManager::fillStatusV2") :
            self.source.index("bool OtaUpdateManager::requestMatchesCache")
        ]
        self.assertIn("status.partition_schema = config_.partition_schema", status)
        self.assertIn("status.running_generation = config_.current_generation", status)
        self.assertRegex(status, r"if \(isTransient\(status\.state\)\) \{")
        for field in (
            "status.transaction_id",
            "status.target_generation",
            "status.image_size",
            "status.target_image_sha256",
            "status.manifest_sha256",
        ):
            self.assertIn(field, status)

    def test_status_v2_uses_boot_cached_running_image_metadata(self) -> None:
        begin = self.source[
            self.source.index("bool OtaUpdateManager::begin") :
            self.source.index("bool OtaUpdateManager::takeRestartRequest")
        ]
        status = self.source[
            self.source.index("bool OtaUpdateManager::fillStatusV2") :
            self.source.index("bool OtaUpdateManager::requestMatchesCache")
        ]
        self.assertIn("esp_partition_get_sha256", begin)
        self.assertIn("esp_ota_get_partition_description", begin)
        self.assertNotIn("esp_partition_get_sha256", status)
        self.assertNotIn("esp_ota_get_partition_description", status)
        self.assertIn("running_image_metadata_valid_", status)

    def test_handle_validates_shared_frame_flags_before_cache_lookup(self) -> None:
        handle = self.source[
            self.source.index("bool OtaUpdateManager::handle") :
            self.source.index("bool OtaUpdateManager::enter")
        ]
        validation = "makcm::ota::validFrameFlags(request.command, request.flags)"
        self.assertIn(validation, handle)
        self.assertLess(
            handle.index(validation), handle.index("requestMatchesCache(request)")
        )
        self.assertIn("fillStatusV2(request, response)", handle)
        self.assertNotIn("Command::StatusResponse :", handle)

    def test_only_current_transfer_mutations_can_clear_transient_state(self) -> None:
        helper_start = self.source.index("bool isTransferMutation")
        helpers = self.source[
            helper_start:self.source.index("bool isCacheable", helper_start)
        ]
        handle = self.source[
            self.source.index("bool OtaUpdateManager::handle") :
            self.source.index("bool OtaUpdateManager::enter")
        ]
        self.assertIn("isTransferMutation", helpers)
        for command in ("Manifest", "Data", "Prepare", "Activate"):
            self.assertIn(f"Command::{command}", helpers)
        for command in (
            "Hello", "Status", "ExitMaintenance", "Abort", "Confirm",
            "EnterMaintenance", "Ack", "Nack", "StatusResponse",
        ):
            self.assertNotIn(f"Command::{command}", helpers)
        clear = handle[handle.index("if (!ok &&") : handle.index("clearTransfer(now_ms)")]
        self.assertIn("valid_flags", clear)
        self.assertIn("request.target == config_.role", clear)
        self.assertIn("request.transaction_id == transaction_id_", clear)
        self.assertIn("isTransferMutation(request.command)", clear)

    def test_no_retired_ota_states_remain_in_left_integration(self) -> None:
        combined = self.header + self.source + self.app
        self.assertIsNone(
            re.search(r"makcm::ota::State::(?:Fault|Maintenance|Confirmed|ActivationPending)", combined)
        )


if __name__ == "__main__":
    unittest.main()
