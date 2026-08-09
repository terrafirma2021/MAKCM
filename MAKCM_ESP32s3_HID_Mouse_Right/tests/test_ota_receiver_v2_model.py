"""Four-state Status V2 and source-contract checks for the right receiver."""

from pathlib import Path
import hashlib
import unittest


ROOT = Path(__file__).resolve().parents[2]
HEADER = ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / "include" / "OtaReceiver.h"
SOURCE = ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / "src" / "ota_receiver.cpp"


class StatusModel:
    IDLE, RECEIVING, PREPARED, PENDING_CONFIRM = range(4)

    def __init__(self, state=IDLE):
        self.state = state
        self.transaction_id = 0
        self.target_generation = 0
        self.image_size = 0
        self.next_offset = 0
        self.target_slot = 0xFF
        self.target_hash = bytes(32)
        self.manifest_hash = bytes(32)
        self.running_generation = 19
        self.partition_schema = 0x52010001
        self.running_hash = hashlib.sha256(b"running").digest()

    def transient(self, *, tx=7, generation=20, size=4096, offset=0,
                  slot=0x11):
        self.transaction_id = tx
        self.target_generation = generation
        self.image_size = size
        self.next_offset = offset
        self.target_slot = slot
        self.target_hash = hashlib.sha256(b"target").digest()
        self.manifest_hash = hashlib.sha256(b"manifest").digest()
        return self

    def valid(self):
        if self.state not in range(4) or not self.running_generation:
            return False
        if not self.partition_schema or not any(self.running_hash):
            return False
        is_transient = self.state in (self.RECEIVING, self.PREPARED)
        facts = (self.transaction_id and self.target_generation and
                 self.image_size and self.next_offset <= self.image_size and
                 self.target_slot != 0xFF and any(self.target_hash) and
                 any(self.manifest_hash))
        if is_transient and not facts:
            return False
        if self.state == self.PREPARED and self.next_offset != self.image_size:
            return False
        if not is_transient and (self.transaction_id or self.target_generation or
                                 self.image_size or self.next_offset or
                                 self.target_slot != 0xFF or any(self.target_hash) or
                                 any(self.manifest_hash)):
            return False
        return True


class RecoverySessionModel:
    """Models the persisted-in-RAM transaction discovered after host restart."""

    STATUS_V2 = 0x08
    RETRANSMIT = 0x02

    def __init__(self, state, bound_transaction):
        self.state = state
        self.bound_transaction = bound_transaction
        self.maintenance_active = True

    def status(self, query_transaction, flags=STATUS_V2):
        del query_transaction
        if (not self.maintenance_active or
                flags & ~self.RETRANSMIT != self.STATUS_V2):
            return None
        return self.state, self.bound_transaction

    def abort(self, transaction):
        if not self.maintenance_active or transaction != self.bound_transaction:
            return False
        self.state = StatusModel.IDLE
        self.maintenance_active = False
        return True

    def rebind(self, query_transaction):
        if not self.maintenance_active or query_transaction == 0:
            return False
        # The image transaction remains bound_transaction; only the volatile
        # maintenance/query session changes.
        return True


class PendingConfirmDeadlineModel:
    """Models a boot-anchored confirm deadline independent of wire activity."""

    TIMEOUT_MS = 120_000

    def __init__(self, boot_ms):
        self.pending_confirm_started_ms = boot_ms
        self.last_activity_ms = boot_ms

    def traffic(self, now_ms):
        self.last_activity_ms = now_ms

    def should_rollback(self, now_ms):
        return now_ms - self.pending_confirm_started_ms > self.TIMEOUT_MS

    def clear_pending_confirm(self):
        self.pending_confirm_started_ms = 0


class RightFourStateTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.header = HEADER.read_text(encoding="utf-8")
        cls.source = SOURCE.read_text(encoding="utf-8")

    def test_idle_status_has_no_transaction_facts(self):
        self.assertTrue(StatusModel().valid())

    def test_pending_confirm_status_has_no_transaction_facts(self):
        self.assertTrue(StatusModel(StatusModel.PENDING_CONFIRM).valid())

    def test_receiving_status_requires_all_transient_facts(self):
        self.assertFalse(StatusModel(StatusModel.RECEIVING).valid())
        self.assertTrue(StatusModel(StatusModel.RECEIVING).transient().valid())

    def test_prepared_status_requires_complete_image(self):
        model = StatusModel(StatusModel.PREPARED).transient(offset=4095)
        self.assertFalse(model.valid())
        model.next_offset = model.image_size
        self.assertTrue(model.valid())

    def test_non_transient_status_rejects_target_hash(self):
        model = StatusModel()
        model.target_hash = b"x" * 32
        self.assertFalse(model.valid())

    def test_status_always_requires_partition_schema(self):
        model = StatusModel()
        model.partition_schema = 0
        self.assertFalse(model.valid())

    def test_status_always_requires_running_generation(self):
        model = StatusModel()
        model.running_generation = 0
        self.assertFalse(model.valid())

    def test_status_always_requires_running_hash(self):
        model = StatusModel()
        model.running_hash = bytes(32)
        self.assertFalse(model.valid())

    def test_public_state_is_common_four_state_type(self):
        self.assertIn("using State = makcm::ota::State;", self.header)
        self.assertNotIn("enum class State", self.header)

    def test_preferences_and_snapshot_types_are_absent(self):
        combined = self.header + self.source
        for retired in ("Preferences", "OtaSnapshotV2", "snapshot_", "CompletedRecord"):
            self.assertNotIn(retired, combined)

    def test_begin_derives_state_only_from_running_partition(self):
        begin = self.source[self.source.index("void OtaReceiver::begin"):
                            self.source.index("bool OtaReceiver::enterMaintenance")]
        self.assertIn("esp_ota_get_state_partition", begin)
        self.assertIn("ESP_OTA_IMG_PENDING_VERIFY", begin)
        self.assertNotIn("loadPersistent", begin)

    def test_status_is_observational_and_does_not_release_maintenance(self):
        case = self.source[self.source.index("case Command::Status:"):
                           self.source.index("case Command::Activate:")]
        self.assertNotIn("releaseMaintenance", case)
        self.assertNotIn("maintenanceActive_", case)
        self.assertNotIn("sessionTransactionId_", case)
        self.assertIn("fillStatusV2", case)

        fill = self.source[self.source.index("bool OtaReceiver::fillStatusV2"):
                           self.source.index("void OtaReceiver::cacheRequest")]
        self.assertIn("request.length != 0", fill)
        self.assertIn("request.flags & ~kFlagRetransmit", fill)

    def test_status_uses_boot_cached_running_image_hash(self):
        begin = self.source[self.source.index("void OtaReceiver::begin"):
                            self.source.index("bool OtaReceiver::enterMaintenance")]
        fill = self.source[self.source.index("bool OtaReceiver::fillStatusV2"):
                           self.source.index("void OtaReceiver::cacheRequest")]
        self.assertIn("esp_partition_get_sha256", begin)
        self.assertNotIn("esp_partition_get_sha256", fill)
        self.assertIn("runningImageSha256Valid_", fill)

    def test_embedded_identity_scan_prefilters_on_magic(self):
        scan = self.source[self.source.index("bool verifyEmbeddedIdentity"):
                           self.source.index("bool isOtaApplication")]
        loop = scan[scan.index("for (uint32_t i = 0;"):]
        magic = loop.index("kEmbeddedIdentityMagic")
        copy = loop.index("memcpy(&identity")
        validate = loop.index("validEmbeddedImageIdentity")
        self.assertLess(magic, copy)
        self.assertLess(copy, validate)

    def test_crash_rerun_observes_old_transaction_then_aborts_with_old_id(self):
        old_tx = 0x10203040
        new_query_tx = 0x50607080
        for state in (StatusModel.RECEIVING, StatusModel.PREPARED):
            with self.subTest(state=state):
                receiver = RecoverySessionModel(state, old_tx)
                self.assertTrue(receiver.rebind(new_query_tx))
                self.assertEqual(receiver.status(new_query_tx), (state, old_tx))
                self.assertEqual(
                    receiver.status(new_query_tx,
                                    RecoverySessionModel.STATUS_V2 |
                                    RecoverySessionModel.RETRANSMIT),
                    (state, old_tx),
                )
                self.assertIsNone(receiver.status(new_query_tx, 0))
                self.assertFalse(receiver.abort(new_query_tx))
                self.assertTrue(receiver.abort(old_tx))
                self.assertEqual(receiver.state, StatusModel.IDLE)

    def test_active_maintenance_rebind_preserves_image_transaction(self):
        enter = self.source[self.source.index("bool OtaReceiver::enterMaintenance"):
                            self.source.index("bool OtaReceiver::peerCompatible")]
        self.assertIn("if (isMaintenanceActive())", enter)
        self.assertIn("sessionTransactionId_ = transactionId", enter)
        self.assertNotIn("ActiveTransactionMismatch", enter)
        self.assertNotIn("transactionId_ = transactionId", enter)

    def test_abort_is_an_explicit_destructive_session_release(self):
        case = self.source[self.source.index("case Command::Abort:"):
                           self.source.index("case Command::Confirm:")]
        self.assertIn("transactionId_", case)
        self.assertIn("sessionTransactionId_", case)
        self.assertIn("releaseMaintenanceAfterResponse", case)
        generic_abort = self.source[
            self.source.index("if (!accepted &&"):
            self.source.index("lastError_ = accepted")
        ]
        self.assertIn("abortTransferOnFailure", generic_abort)

    def test_exit_maintenance_ack_precedes_release_and_never_aborts_state(self):
        case = self.source[self.source.index("case Command::ExitMaintenance:"):
                           self.source.index("case Command::Confirm:")]
        self.assertIn("frame.offset != 0", case)
        self.assertIn("frame.length != 0", case)
        self.assertIn("frame.transaction_id == 0", case)
        self.assertNotIn("sessionTransactionId_", case)
        self.assertIn("accepted = true", case)
        self.assertNotIn("abortUpdate", case)
        tail = self.source[self.source.index("fillResponse(frame"):
                           self.source.index("if (restartAfterResponse)")]
        self.assertLess(tail.index("emitResponse"),
                        tail.index("releaseMaintenance"))

    def test_only_matched_transfer_mutation_failures_abort_transient_state(self):
        handle = self.source[self.source.index("void OtaReceiver::handleFrame"):
                             self.source.index("void OtaReceiver::feed")]
        self.assertIn("bool abortTransferOnFailure = false;", handle)
        self.assertIn("frame.transaction_id == transactionId_", handle)
        generic_abort = handle[handle.index("if (!accepted &&"):
                               handle.index("lastError_ = accepted")]
        self.assertIn("abortTransferOnFailure", generic_abort)

        feed = self.source[self.source.index("void OtaReceiver::feed"):
                           self.source.index("void OtaReceiver::tick")]
        self.assertNotIn("abortUpdate", feed)

    def test_manifest_is_idle_maintenance_only(self):
        case = self.source[self.source.index("case Command::Manifest:"):
                           self.source.index("case Command::Data:")]
        self.assertIn("state_ != State::Idle", case)
        self.assertIn("!isMaintenanceActive()", case)

    def test_data_requires_exact_next_offset(self):
        data = self.source[self.source.index("bool OtaReceiver::handleData"):
                           self.source.index("bool OtaReceiver::prepareImage")]
        self.assertIn("offset != bytesWritten_", data)
        self.assertNotIn("esp_partition_read(targetPartition_, offset, s_compareBuffer", data)

    def test_only_exact_retransmits_use_ram_response_cache(self):
        self.assertIn("requestMatchesCache", self.source)
        self.assertIn("kFlagRetransmit", self.source)
        self.assertNotIn("replayCommand", self.source)

    def test_prepare_uses_hash_end_and_embedded_identity_checks(self):
        prepare = self.source[self.source.index("bool OtaReceiver::prepareImage"):
                              self.source.index("bool OtaReceiver::handleActivate")]
        self.assertIn("esp_ota_end", prepare)
        self.assertIn("verifyEmbeddedIdentity", prepare)
        self.assertIn("memcmp(digest, imageSha256_", prepare)

    def test_activate_sets_boot_partition_before_restart(self):
        activate = self.source[self.source.index("bool OtaReceiver::handleActivate"):
                               self.source.index("bool OtaReceiver::handleConfirm")]
        self.assertIn("esp_ota_set_boot_partition", activate)
        handle = self.source[self.source.index("void OtaReceiver::handleFrame"):]
        self.assertLess(handle.index("emitResponse"), handle.index("esp_restart()"))

    def test_confirm_marks_valid_only_after_both_health_checks(self):
        confirm = self.source[self.source.index("bool OtaReceiver::handleConfirm"):
                              self.source.index("void OtaReceiver::handleFrame")]
        self.assertIn("localHealthPassed()", confirm)
        self.assertIn("healthPassed()", confirm)
        self.assertIn("esp_ota_mark_app_valid_cancel_rollback", confirm)

    def test_pending_abort_and_deadline_use_official_rollback(self):
        self.assertGreaterEqual(
            self.source.count("esp_ota_mark_app_invalid_rollback_and_reboot"), 2)
        tick = self.source[self.source.index("void OtaReceiver::tick()") :]
        self.assertIn("state_ == State::Prepared", tick)
        self.assertIn("releaseMaintenance();", tick)

    def test_pending_confirm_deadline_is_not_extended_by_protocol_traffic(self):
        model = PendingConfirmDeadlineModel(1_000)
        model.traffic(120_000)
        self.assertTrue(model.should_rollback(121_001))
        self.assertEqual(model.pending_confirm_started_ms, 1_000)

    def test_pending_confirm_deadline_source_is_boot_anchored_and_cleared(self):
        self.assertIn("uint32_t pendingConfirmStartedMs_ = 0;", self.header)

        begin = self.source[self.source.index("void OtaReceiver::begin"):
                            self.source.index("bool OtaReceiver::enterMaintenance")]
        self.assertIn("pendingConfirmStartedMs_ =", begin)
        self.assertIn("state_ == State::PendingConfirm ? lastActivityMs_ : 0;",
                      begin)
        self.assertIn("ESP_OTA_IMG_PENDING_VERIFY", begin)
        self.assertEqual(begin.count("pendingConfirmStartedMs_ ="), 1)

        tick = self.source[self.source.index("void OtaReceiver::tick()") :]
        pending_branch = tick.split("const uint32_t elapsed", 1)[0]
        self.assertIn("now - pendingConfirmStartedMs_", pending_branch)
        self.assertNotIn("lastActivityMs_", pending_branch)

        enter = self.source[self.source.index("bool OtaReceiver::enterMaintenance"):
                            self.source.index("bool OtaReceiver::peerCompatible")]
        status = self.source[self.source.index("bool OtaReceiver::fillStatusV2"):
                             self.source.index("void OtaReceiver::cacheRequest")]
        feed = self.source[self.source.index("void OtaReceiver::feed"):
                           self.source.index("void OtaReceiver::tick()")]
        for traffic in (enter, status, feed):
            self.assertNotIn("pendingConfirmStartedMs_", traffic)

        handle = self.source[self.source.index("void OtaReceiver::handleFrame"):
                             self.source.index("void OtaReceiver::feed")]
        self.assertEqual(handle.count("pendingConfirmStartedMs_"), 1)
        self.assertIn("pendingConfirmStartedMs_ = 0;", handle)

        confirm = self.source[self.source.index("bool OtaReceiver::handleConfirm"):
                              self.source.index("void OtaReceiver::handleFrame")]
        reset = self.source[self.source.index("void OtaReceiver::resetTransient"):
                            self.source.index("bool OtaReceiver::abortUpdate")]
        self.assertIn("pendingConfirmStartedMs_ = 0;", confirm)
        self.assertIn("pendingConfirmStartedMs_ = 0;", reset)
        self.assertGreaterEqual(tick.count("pendingConfirmStartedMs_ = 0;"), 1)

    def test_no_persistent_or_legacy_state_names_remain(self):
        for retired in ("State::Activated", "State::AwaitConfirm",
                        "State::Failed", "State::Confirmed",
                        "State::ActivateIntent", "tombstone", "ledger",
                        "migration"):
            self.assertNotIn(retired, self.header + self.source)


if __name__ == "__main__":
    unittest.main()
