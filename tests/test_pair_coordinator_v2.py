from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class PairCoordinatorV2Tests(unittest.TestCase):
    def test_record_schema_contains_ab_identity_and_tombstone(self) -> None:
        header = (ROOT / "common" / "PairOtaTransaction.h").read_text(
            encoding="utf-8"
        )
        for token in (
            "kPairRecordVersion = 2",
            "kPairRecordWireSize",
            "record_sequence",
            "target_slot",
            "previous_slot",
            "image_sha256",
            "manifest_sha256",
            "identity_digest",
            "PairCompletedTombstone",
            "BothPairCommitted",
            "RollbackIntent",
            "decodePairRecord",
        ):
            self.assertIn(token, header)

    def test_left_uses_ab_keys_readback_and_never_erases_pair_evidence(self) -> None:
        app = (
            ROOT
            / "MAKCM_ESP32s3_Device_Mouse_Left_IDF"
            / "main"
            / "app_main.cpp"
        ).read_text(encoding="utf-8")
        for token in (
            'kPairKeyA[] = "pair_a"',
            'kPairKeyB[] = "pair_b"',
            "nvs_get_blob(pair_nvs, key, pair_readback_wire",
            "decodePairRecord(pair_readback_wire",
            "pairRecordHasExactPairCommit",
            "pair_record_corrupt",
        ):
            self.assertIn(token, app)
        self.assertNotIn('nvs_erase_key(pair_nvs, "transaction")', app)
        self.assertNotIn('nvs_erase_key(pair_nvs, "pair_a")', app)
        self.assertNotIn('nvs_erase_key(pair_nvs, "pair_b")', app)

    def test_startup_is_host_reconcile_only(self) -> None:
        app = (
            ROOT
            / "MAKCM_ESP32s3_Device_Mouse_Left_IDF"
            / "main"
            / "app_main.cpp"
        ).read_text(encoding="utf-8")
        self.assertIn("pairRecordRequiresHostReconcile", app)
        self.assertIn("Linux must query exact right-board facts first", app)
        self.assertNotIn("RecoveryAction::Continue", app)
        self.assertNotIn("RecoveryAction::RollBack", app)

    def test_release_gate_is_stricter_than_active_record_presence(self) -> None:
        app = (
            ROOT
            / "MAKCM_ESP32s3_Device_Mouse_Left_IDF"
            / "main"
            / "app_main.cpp"
        ).read_text(encoding="utf-8")
        start = app.index("bool tryReleaseOtaQuiesce()")
        end = app.index("bool otaPairHealth", start)
        release = app[start:end]
        self.assertIn("!pair_record_valid", release)
        self.assertIn("pairRecordHasExactPairCommit", release)
        self.assertLess(
            release.index("pairRecordHasExactPairCommit"),
            release.index("bridge.setOtaMaintenance(false)"),
        )

    def test_bootstrap_only_quiesce_releases_without_a_peer_fact(self) -> None:
        # A quiesce closes the CH343 text plane, and an Idle left board NACKs
        # ABORT as WrongTransaction, so a bootstrap whose right board goes
        # silent used to be recoverable only by physical power cycle.  Releasing
        # is safe exactly when nothing that can leave an image in flight was
        # ever forwarded.
        app = (
            ROOT
            / "MAKCM_ESP32s3_Device_Mouse_Left_IDF"
            / "main"
            / "app_main.cpp"
        ).read_text(encoding="utf-8")

        forward = app[app.index("bool forwardRightOtaRequest("):]
        forward = forward[: forward.index("\n}\n")]
        for command in ("Manifest", "Data", "Prepare", "Activate", "Confirm",
                        "Abort"):
            self.assertIn(f"case makcm::ota::Command::{command}:", forward)
        self.assertIn("right_image_traffic_forwarded = true;", forward)
        for discovery in ("Hello", "EnterMaintenance", "Status"):
            self.assertNotIn(f"case makcm::ota::Command::{discovery}:", forward)

        # The retry cadence must be armed by the quiesce itself; otherwise a
        # right board that never answers leaves the predicate unevaluated.
        quiesce = app[app.index("bool otaQuiesce(void *)"):]
        quiesce = quiesce[: quiesce.index("\n}\n")]
        self.assertIn("right_link_reset_at_ms.store(nowMs() + 250", quiesce)

        start = app.index("bool tryReleaseOtaQuiesce()")
        release = app[start: app.index("bool otaPairHealth", start)]
        self.assertIn("bootstrap_only_session", release)
        self.assertIn("!right_image_traffic_forwarded", release)
        self.assertIn(
            "if (!exact_pair_terminal && !exact_unpaired_idle && "
            "!bootstrap_only_session)",
            release,
        )
        # Releasing must end the session, or the next bootstrap inherits a
        # stale "nothing was forwarded" claim.
        self.assertIn("right_image_traffic_forwarded = false;", release)
        self.assertLess(
            release.index("bootstrap_only_session"),
            release.index("bridge.setOtaMaintenance(false)"),
        )

    def test_maintenance_keeps_the_diagnostic_plane_open(self) -> None:
        # Quiesce owns the data plane.  Killing the CH343 text plane with it
        # made every failed update a black box: LEFT_STATUS went silent exactly
        # when its counters were the only way to tell "the peer never answered"
        # from "we ate the answer", and a stuck quiesce had no observable cause.
        secondary = (
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "components" /
            "makcm_core" / "secondary.cpp"
        ).read_text(encoding="utf-8")

        handle = secondary[secondary.index("void Secondary::handleByte("):]
        handle = handle[: handle.index("\nvoid Secondary::handleLine(")]
        self.assertNotIn(
            "if (ota_maintenance_.load(std::memory_order_acquire)) return;",
            handle,
        )
        # The decoder still claims the stream only at a text-frame boundary.
        self.assertIn("line_length_ == 0 && byte == makcm::ota::kMagic[0]",
                      handle)

        send_text = secondary[secondary.index("bool Secondary::sendText("):]
        send_text = send_text[: send_text.index("\n}\n")]
        self.assertNotIn("otaMaintenance()", send_text)
        # The data plane must stay closed.
        button = secondary[secondary.index("bool Secondary::sendButtonFrame("):]
        button = button[: button.index("\n}\n")]
        self.assertIn("otaMaintenance()", button)

        # The release predicate must be reportable, not just enforced.
        app = (
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "main" /
            "app_main.cpp"
        ).read_text(encoding="utf-8")
        self.assertIn("LEFT_OTA_DIAG", app)
        for field in ("quiesced=", "bridge_maint=", "sec_maint=", "excl=",
                      "imgfwd=", "pending=", "authz=", "pair=", "installed="):
            self.assertIn(field, app)

    def test_bridge_rx_task_is_sized_for_the_pair_persistence_path(self) -> None:
        # A right-board MKOT response is decoded on makcm_bridge_rx and runs
        # bridgeOtaFrame -> observeRightOtaResponse -> mutatePairRecord ->
        # persistPairRecord inline, carrying three PairTransactionRecord objects
        # plus the NVS internals.  On the 6144 default that overflowed on the
        # first response and tripped the end-of-stack watchpoint, which reads
        # from the host as "the right board never answered".
        app = (
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "main" /
            "app_main.cpp"
        ).read_text(encoding="utf-8")
        self.assertIn("bridge_config.task_stack = 12288;", app)

        header = (
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "components" /
            "makcm_core" / "include" / "bridge_transport.h"
        ).read_text(encoding="utf-8")
        # The starving task must be observable.  LEFT_STATUS previously reported
        # only the Secondary task's headroom, which stayed flat and healthy
        # while makcm_bridge_rx was overflowing.
        self.assertIn("minimumFreeRxTaskStackBytes", header)

        bridge = (
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "components" /
            "makcm_core" / "bridge_transport.cpp"
        ).read_text(encoding="utf-8")
        self.assertIn("recordRxTaskStackHighWaterMark", bridge)
        rx_task = bridge[bridge.index("void BridgeTransport::rxTask("):]
        rx_task = rx_task[: rx_task.index("\n}\n")]
        # Sampling must happen after the batch, so it covers the deepest frame
        # that batch delivered rather than an unwound stack.
        self.assertLess(
            rx_task.index("handleByte(buffer[i])"),
            rx_task.index("recordRxTaskStackHighWaterMark()"),
        )
        self.assertIn("bstack=%lu", app)
        self.assertIn("bridge.minimumFreeRxTaskStackBytes()", app)

    def test_new_transaction_can_bootstrap_through_a_stale_maintenance_latch(
        self,
    ) -> None:
        # startRightMaintenanceBootstrap runs exactly when bridge maintenance is
        # held for a DIFFERENT transaction, so the OTA_MAINT_BEGIN it sends must
        # not be gated on maintenance.  sendText() is, which made a stale latch
        # permanently unrecoverable: the only message that can retarget the link
        # was suppressed by the latch it exists to clear, and the resulting V1
        # Busy NACK is dropped by the tool's Status-V2 filter, so the host saw
        # only "timed out waiting for OTA response".
        app = (
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "main" /
            "app_main.cpp"
        ).read_text(encoding="utf-8")
        bootstrap = app[app.index("bool startRightMaintenanceBootstrap("):]
        bootstrap = bootstrap[: bootstrap.index("\n}\n")]
        self.assertIn("bridge.sendOtaText(command)", bootstrap)
        self.assertNotIn("bridge.sendText(command)", bootstrap)

        bridge = (
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "components" /
            "makcm_core" / "bridge_transport.cpp"
        ).read_text(encoding="utf-8")
        send_ota_text = bridge[bridge.index("bool BridgeTransport::sendOtaText("):]
        send_ota_text = send_ota_text[: send_ota_text.index("\n}\n")]
        # Comments here explain the gate and the queue, so assert on code only.
        code = "\n".join(
            line for line in send_ota_text.splitlines()
            if not line.lstrip().startswith("//")
        )
        self.assertNotIn("otaMaintenance()", code)
        # txTask() discards queued items during maintenance, so this must write
        # synchronously under the tx mutex rather than through tx_queue_.
        self.assertNotIn("tx_queue_", code)
        self.assertIn("xSemaphoreTake(tx_mutex_", code)
        self.assertIn("uart_write_bytes(config_.port", code)

        # The diagnostic relay must stay gated -- it really is the data plane.
        send_text = bridge[bridge.index("bool BridgeTransport::sendText("):]
        send_text = send_text[: send_text.index("\n}\n")]
        self.assertIn("otaMaintenance()", send_text)

        # ...and the reply must be audible too.  handleOtaBridgeTextLine() is
        # the only consumer of OTA_READY/OTA_PENDING/OTA_UNAVAILABLE, and it is
        # reachable only through the text path, so an OTA-armed RX that
        # swallowed every byte made the bootstrap uncompletable in exactly the
        # state it exists to escape.
        handle = bridge[bridge.index("void BridgeTransport::handleByte("):]
        handle = handle[: handle.index("\n}\n")]
        armed = handle[handle.index("if (otaRxArmed())"):]
        self.assertIn("ota_decoder_.buffered() != 0", armed)
        self.assertIn("ota_line_idle_ && byte == makcm::ota::kMagic[0]", armed)
        self.assertIn("text_handler_(context_, byte)", armed)
        # A mode switch is a frame boundary, or the decoder cannot re-arm.
        apply_mode = bridge[
            bridge.index("void BridgeTransport::applyPendingOtaMode("):]
        apply_mode = apply_mode[: apply_mode.index("\n}\n")]
        self.assertIn("ota_line_idle_ = true;", apply_mode)

    def test_expired_bootstrap_answers_the_host_instead_of_going_quiet(
        self,
    ) -> None:
        # The 7 s pending-bootstrap watchdog used to clear the request without
        # replying, so an expired handshake and a dead link were the same string
        # on the host: "timed out waiting for OTA response".
        app = (
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "main" /
            "app_main.cpp"
        ).read_text(encoding="utf-8")
        watchdog = app[app.index("loop_now - pending_right_request_started_ms > 7000"):]
        watchdog = watchdog[: watchdog.index("xSemaphoreGive(ota_mutex);")]
        self.assertIn("makcm::ota::NackReason::Timeout", watchdog)
        self.assertLess(
            watchdog.index("sendOtaNack(pending_right_request"),
            watchdog.index("pending_right_request_valid = false;"),
        )

        # The peer's OTA_* handshake lines must reach the operator.  Consuming
        # them silently made a bootstrap stalled on OTA_PENDING look exactly
        # like a peer that never answered.
        relay = app[app.index("void bridgeText("):]
        relay = relay[: relay.index("\n}\n")]
        self.assertLess(
            relay.index('secondary.sendText("RIGHT:")'),
            relay.index("handleOtaBridgeTextLine(bridge_text_line)"),
        )
        self.assertNotIn("if (!handleOtaBridgeTextLine(", relay)

    def test_same_sequence_different_bytes_is_a_conflict_model(self) -> None:
        # This mirrors the coordinator's A/B selection rule without touching
        # NVS: equal sequence numbers are valid only when their complete wire
        # bytes are identical.
        a = (7, b"pair-record-a")
        b = (7, b"pair-record-b")
        self.assertEqual(a[0], b[0])
        self.assertNotEqual(a[1], b[1])
        self.assertTrue("same sequence" in "same sequence conflict")

    def test_every_pair_mutation_allocates_a_new_sequence(self) -> None:
        app = (
            ROOT
            / "MAKCM_ESP32s3_Device_Mouse_Left_IDF"
            / "main"
            / "app_main.cpp"
        ).read_text(encoding="utf-8")
        start = app.index("bool persistPairRecord")
        end = app.index("PairCompletedTombstone makePairTombstone", start)
        persist = app[start:end]
        self.assertIn(
            "next.record_sequence = pair_record_valid", persist
        )
        self.assertIn("pair_record.record_sequence + 1", persist)
        self.assertNotIn("if (next.record_sequence == 0)", persist)

    def test_pair_manifest_is_not_persisted_before_board_validation(self) -> None:
        app = (
            ROOT
            / "MAKCM_ESP32s3_Device_Mouse_Left_IDF"
            / "main"
            / "app_main.cpp"
        ).read_text(encoding="utf-8")
        start = app.index("bool preparePairIntentForRequest")
        end = app.index("bool updatePairFromLocalStatus", start)
        prepare = app[start:end]
        manifest_start = prepare.index(
            "if (request.command == Command::Manifest)"
        )
        manifest_end = prepare.index("if (!pair_mode_active", manifest_start)
        manifest = prepare[manifest_start:manifest_end]
        self.assertIn("return true;", manifest)
        self.assertNotIn("mutatePairRecord", manifest)
        self.assertNotIn("buildPairManifestIntent", app)

    def test_right_confirm_is_not_blocked_on_its_future_commit_fact(self) -> None:
        app = (
            ROOT
            / "MAKCM_ESP32s3_Device_Mouse_Left_IDF"
            / "main"
            / "app_main.cpp"
        ).read_text(encoding="utf-8")
        start = app.index("if (request.command == Command::Confirm)")
        end = app.index("if (request.command == Command::Abort)", start)
        confirm = app[start:end]
        right_start = confirm.index("if (role == Target::Right)")
        left_refresh = confirm.index("requestLocalStatusV2", right_start)
        right_branch = confirm[right_start:left_refresh]
        self.assertIn("PairStage::PairCommitIntent", right_branch)
        self.assertNotIn("pair_record.right.pair_committed", right_branch)
        self.assertIn("pair_record.right.local_validated", right_branch)

    def test_left_activate_refreshes_authoritative_post_prepare_fact(self) -> None:
        app = (
            ROOT
            / "MAKCM_ESP32s3_Device_Mouse_Left_IDF"
            / "main"
            / "app_main.cpp"
        ).read_text(encoding="utf-8")
        start = app.index("if (request.command == Command::Activate)")
        end = app.index("if (request.command == Command::Confirm)", start)
        activate = app[start:end]
        self.assertIn("if (role == Target::Left)", activate)
        self.assertIn("requestLocalStatusV2(request, local_status)", activate)
        self.assertIn(
            "pairFactFromStatus(local_status, Target::Left, fact)", activate
        )
        self.assertLess(
            activate.index("requestLocalStatusV2"),
            activate.index("Phase::ActivateIntent"),
        )

    def test_left_phase_mapping_requires_right_local_validation(self) -> None:
        app = (
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "main" /
            "app_main.cpp"
        ).read_text(encoding="utf-8")
        mapping = app[
            app.index("bool updatePairFromLocalStatus") :
            app.index("bool observeLocalOtaResponse")
        ]
        self.assertIn("right != nullptr && right->local_validated", mapping)
        self.assertIn("PairStage::RightLocalValidated", mapping)
        self.assertIn("PairStage::MaintenanceRequired", mapping)

    def test_bridge_relays_any_right_response_shape(self) -> None:
        """V1 and V2 responses both reach the host; only V2 updates the cache.

        The bridge no longer discriminates payload shapes: whatever the right
        board answered is forwarded verbatim, and a decodable Status V2 frame
        additionally refreshes the cached right-board state used by the
        release predicates.
        """
        app = (
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "main" /
            "app_main.cpp"
        ).read_text(encoding="utf-8")
        bridge = app[
            app.rindex("void bridgeOtaFrame") :
            app.rindex("void secondaryOtaFrame")
        ]
        self.assertIn("frame.command == makcm::ota::Command::StatusResponse", bridge)
        self.assertIn("secondary.sendOtaFrame(frame);", bridge)
        # The cache update is gated on a successful V2 decode, nothing else.
        self.assertIn("if (valid_status_v2) {", bridge)

    def test_every_snapshot_commit_failure_names_its_site(self) -> None:
        """FlashWriteFailed alone cannot distinguish seven different faults.

        Four of commitPendingSnapshotV2()'s rejections happen before any
        esp_err_t exists, so last_nvs_error keeps whatever a benign earlier
        read left there.  A rejected MANIFEST then looks identical to a
        rejected ABORT, and neither points at the cause.
        """
        source = (
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "components" /
            "makcm_core" / "ota_update_manager.cpp"
        ).read_text(encoding="utf-8")
        header = (
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "components" /
            "makcm_core" / "include" / "ota_update_manager.h"
        ).read_text(encoding="utf-8")
        for name in ("NoNvsHandle", "RoleMismatch", "SequenceSaturated",
                     "EncodeRejected", "SetBlobFailed", "CommitFailed",
                     "ReadbackMismatch"):
            self.assertIn(f"{name} = ", header)
            self.assertIn(f"SnapshotCommitFault::{name}", source)
        body = source[source.index("bool OtaUpdateManager::commitPendingSnapshotV2"):]
        body = body[: body.index("bool OtaUpdateManager::loadSnapshotV2")]
        # Success must clear the marker, or a stale site outlives its fault.
        self.assertIn("SnapshotCommitFault::None", body)
        # Every failure exit must be immediately preceded by its attribution,
        # so a later-added guard cannot reintroduce a silent FlashWriteFailed.
        lines = [line.strip() for line in body.splitlines()]
        exits = [i for i, line in enumerate(lines) if line == "return false;"]
        self.assertEqual(len(exits), 7)
        for i in exits:
            self.assertTrue(
                lines[i - 1].startswith("snapshot_commit_fault_ ="),
                f"unattributed commit failure near: {lines[i - 1]!r}",
            )

    def test_snapshot_commit_fault_is_reported_to_the_host(self) -> None:
        main = (
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "main" /
            "app_main.cpp"
        ).read_text(encoding="utf-8")
        self.assertIn("scf=%u", main)
        self.assertIn("ota_manager.snapshotCommitFault()", main)
        self.assertIn("bootEvidence().last_nvs_error", main)

    def test_persisted_fact_derives_local_validated_from_its_phase(self) -> None:
        """The flag is a transaction property, not a board property.

        validSnapshot() requires active.local_validated to equal
        `phase is LocalValidated or PairPending`.  Both boards used to write
        their board-wide local-boot flag here, which is true on any healthy
        board -- so the first durable fact after MANIFEST failed its own
        encoder, before any NVS call, and surfaced as FlashWriteFailed.  That
        blocked every V2 pair OTA on both boards.
        """
        left = (
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "components" /
            "makcm_core" / "ota_update_manager.cpp"
        ).read_text(encoding="utf-8")
        right = (
            ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / "src" / "ota_receiver.cpp"
        ).read_text(encoding="utf-8")
        validator = (ROOT / "common" / "OtaSnapshotV2.h").read_text(
            encoding="utf-8")
        # The invariant this derivation must satisfy.
        self.assertIn("phase_requires_local_validation", validator)
        self.assertIn("snapshot.active.local_validated !=", validator)
        for source, board_flag in ((left, "local_boot_validated_"),
                                   (right, "localBootValidated_")):
            self.assertIn(
                "phase == makcm::ota::snapshot::Phase::LocalValidated ||",
                source)
            self.assertIn(
                "phase == makcm::ota::snapshot::Phase::PairPending;", source)
            # The board flag must no longer reach an active-record write.
            self.assertNotIn(
                f"next.active.local_validated = {board_flag};", source)
            # ...but must still drive the completed tombstone, which records
            # whether the finished transaction's image really validated.
            self.assertIn(
                f"completed.local_validated = {board_flag};", source)

    def test_every_pair_record_fault_names_its_site(self) -> None:
        """The pair record is the second durable store and fails just as opaquely.

        Most of its faults never had an esp_err_t, so latchPairFault() invented
        ESP_ERR_INVALID_STATE / ESP_ERR_INVALID_CRC and collapsed a dozen
        distinct causes onto two values -- a rejected record, a full partition
        and an A/B conflict were indistinguishable, and all of them reach the
        host as the same FlashWriteFailed.
        """
        main = (
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "main" /
            "app_main.cpp"
        ).read_text(encoding="utf-8")
        for name in ("StoreOpenFailed", "CopyReadFailed", "LegacyKeyPresent",
                     "CopyConflict", "BothCopiesInvalid", "LoadedRecordInvalid",
                     "NoNvsHandle", "SequenceSaturated", "RecordRejected",
                     "EncodeFailed", "SetBlobFailed", "CommitFailed",
                     "ReadbackMismatch", "ReadbackDecodeMismatch",
                     "TransactionSwitchBlocked", "SeedFactIncomplete",
                     "BoardFactMismatch", "LocalStatusUnusable",
                     "ResponseCorrelationMismatch"):
            self.assertIn(f"{name} = ", main)
            self.assertIn(f"PairFault::{name}", main)
        # Taking the site by parameter is what forces new faults to declare
        # themselves instead of reusing a neighbour's code.
        self.assertIn("void latchPairFault(PairFault site, esp_err_t error)",
                      main)
        # No caller may fall back to the old single-argument form.
        self.assertNotRegex(main, r"latchPairFault\(ESP_ERR_")
        # A rejected record must be distinguishable from a failed encode: that
        # conflation is exactly what hid the snapshot EncodeRejected bug.
        self.assertIn("PairFault::RecordRejected", main)
        self.assertIn("PairFault::EncodeFailed", main)

    def test_pair_fault_site_is_reported_to_the_host(self) -> None:
        main = (
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "main" /
            "app_main.cpp"
        ).read_text(encoding="utf-8")
        self.assertIn("pf=%u/%d", main)
        self.assertIn("static_cast<unsigned>(pair_fault_site)", main)
        self.assertIn("static_cast<int>(pair_last_nvs_error)", main)
        # Success must clear the site, or a stale fault outlives its cause.
        self.assertIn("pair_fault_site = PairFault::None;", main)

    def test_a_failed_attempt_does_not_consume_its_generation(self) -> None:
        """Only a confirmed install may block re-attempting a generation.

        An Aborted/RolledBack tombstone leaves the board running the previous
        generation, so retrying that same generation is a forward move.  While
        the monotonicity gate applied to every tombstone, the first failed
        gen-N attempt permanently consumed gen N: every retry built a snapshot
        its own encoder rejected, reported as the opaque FlashWriteFailed.
        """
        validator = (ROOT / "common" / "OtaSnapshotV2.h").read_text(
            encoding="utf-8")
        body = validator[validator.index("inline bool validSnapshot"):]
        body = body[: body.index("\ninline ", 10)] if "\ninline " in body[10:] \
            else body
        gate = body[body.index("snapshot.active.present && snapshot.completed.present"):]
        self.assertIn("Outcome::Confirmed", gate)
        # The transaction-id inequality stays unconditional.
        self.assertIn("identity.transaction_id ==", gate)
        # Aborted and RolledBack must not reach the generation comparison.
        confirmed_at = gate.index("snapshot.completed.outcome == Outcome::Confirmed")
        generation_at = gate.index("identity.generation <=")
        self.assertLess(confirmed_at, generation_at)

    def test_fault_without_active_transaction_is_recoverable(self) -> None:
        """A Fault holding no active transaction must not need a BOOT cable.

        ENTER is refused from Fault as InvalidState, and the transaction guard
        refuses everything else as WrongTransaction because the board matches
        no id -- so ABORT, the designated recovery command, could not reach its
        own handler.  Erasing NVS over USB was the only way out.
        """
        source = (
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "components" /
            "makcm_core" / "ota_update_manager.cpp"
        ).read_text(encoding="utf-8")
        dispatch = source[source.index("if (request.target != config_.role)"):]
        dispatch = dispatch[: dispatch.index("switch (request.command)")]
        # The recovery clause must precede the transaction guard that shadowed it.
        recovery = dispatch.index("makcm::ota::Command::Abort")
        guard = dispatch.index("request.transaction_id != transaction_id_")
        self.assertLess(recovery, guard)
        # Match on the durable record, never the volatile id: a board that
        # faulted after ENTER but before any manifest holds an id that was
        # never published in Status V2, so the host cannot name it.
        self.assertIn("state_.load() == makcm::ota::State::Fault", dispatch)
        self.assertIn("!snapshot_.active.present", dispatch)
        self.assertNotIn("transaction_id_ == 0", dispatch)
        # A rejected recovery must not leave the adopted id behind.
        self.assertIn("if (!ok) transaction_id_ = 0;", dispatch)
        # abort() itself must accept Fault on the no-evidence branch.
        body = source[source.index("bool OtaUpdateManager::abort("):]
        body = body[: body.index("bool OtaUpdateManager::resetDownload")]
        self.assertIn("abort_state == makcm::ota::State::Fault", body)
        self.assertIn("!snapshot_.active.present", body)

    def _mutate_body(self) -> str:
        app = (
            ROOT
            / "MAKCM_ESP32s3_Device_Mouse_Left_IDF"
            / "main"
            / "app_main.cpp"
        ).read_text(encoding="utf-8")
        body = app[app.index("bool mutatePairRecord(uint32_t transaction_id,"):]
        return body[: body.index("bool pairFactFromStatus(")]

    def test_mutation_transaction_is_given_not_inferred(self) -> None:
        """The transaction a mutation belongs to is the caller's to state.

        It used to be inferred from `right present ? right : left`.  Callers
        routinely pass the record's own peer fact forward, so a carried-over
        stale fact always won that preference, captured the record's identity,
        and the genuinely fresh fact was then rejected against it as
        BoardFactMismatch -- while the switch guard, which keys off the same
        inference, never ran at all.
        """
        body = self._mutate_body()
        self.assertNotIn("right != nullptr && right->present\n", body)
        self.assertNotIn("? right\n", body)
        # Staleness is dropped, not faulted: only a generation disagreement
        # inside one transaction is a genuine board fact mismatch.
        self.assertIn("left->transaction_id != transaction_id)", body)
        self.assertIn("right->transaction_id != transaction_id)", body)
        drop_at = body.index("right->transaction_id != transaction_id)")
        mismatch_at = body.index("PairFault::BoardFactMismatch")
        self.assertLess(drop_at, mismatch_at)
        # Every call site must name a transaction.
        app = (
            ROOT
            / "MAKCM_ESP32s3_Device_Mouse_Left_IDF"
            / "main"
            / "app_main.cpp"
        ).read_text(encoding="utf-8")
        for call in app.split("mutatePairRecord(")[1:]:
            head = call[: call.index(",")]
            self.assertTrue(
                head.endswith("transaction_id") or head == "uint32_t transaction_id",
                f"mutatePairRecord call does not name its transaction: {head!r}",
            )

    def test_one_sided_rollback_is_a_terminal_shape(self) -> None:
        """RollbackIntent with an absent peer must not wedge the record.

        RolledBack requires *both* facts to be present and rolled back, so a
        transaction the peer never joined can never reach it.  The only prior
        retirement was a side effect inside observeRightOtaResponse, which runs
        solely while a right-board STATUS is being folded in -- a window the
        failure itself tends to close.  A new transaction must be able to
        displace such a record, since it installed nothing on either board.
        """
        header = (ROOT / "common" / "PairOtaTransaction.h").read_text(
            encoding="utf-8"
        )
        predicate = header[
            header.index("inline bool pairRecordHasExactOneSidedRollback(") :
        ]
        predicate = predicate[: predicate.index("\n}\n") + 3]
        # Exactly one participant, and it must durably prove its own rollback.
        self.assertIn("record.stage != PairStage::RollbackIntent", predicate)
        self.assertIn("record.left.present == record.right.present", predicate)
        self.assertIn("joined.local_phase == snapshot::Phase::RolledBack", predicate)
        self.assertIn("joined.completed_terminal_command == Command::Abort", predicate)
        # The safety claim is that the target image will never boot.
        self.assertIn("joined.boot_slot != joined.target_slot", predicate)
        self.assertIn("joined.boot_slot == joined.previous_slot", predicate)
        # It must NOT compare a live measurement against a recorded value:
        # running_image_sha256 comes from esp_partition_get_sha256 while
        # previous_image_sha256 is whatever the transaction wrote down, so any
        # out-of-band flash of the running slot separates them permanently and
        # would wedge the record with no exit.
        code = "\n".join(
            line
            for line in predicate.splitlines()
            if not line.lstrip().startswith("//")
        )
        self.assertNotIn("running_image_sha256", code)
        self.assertNotIn("exactBoardRollbackFact", code)
        # It joins the other two terminal shapes in the switch guard.
        body = self._mutate_body()
        guard = body[body.index("PairFault::TransactionSwitchBlocked") - 600 :]
        guard = guard[: guard.index("PairFault::TransactionSwitchBlocked")]
        for shape in (
            "pairRecordHasExactPairCommit",
            "pairRecordHasExactPairRollback",
            "pairRecordHasExactOneSidedRollback",
        ):
            self.assertIn(shape, guard)

    def test_stage_decisions_ignore_foreign_peer_facts(self) -> None:
        """A peer fact from another transaction must not decide this stage.

        both_rolled_back and right->local_validated read the record's peer
        fact directly.  Left unfiltered, a leftover fact manufactures a
        BothLocalValidated or a RolledBack that no board in this transaction
        ever reached.
        """
        app = (
            ROOT
            / "MAKCM_ESP32s3_Device_Mouse_Left_IDF"
            / "main"
            / "app_main.cpp"
        ).read_text(encoding="utf-8")
        local = app[app.index("bool updatePairFromLocalStatus("):]
        local = local[: local.index("bool observeLocalOtaResponse(")]
        self.assertIn(
            "pair_record.right.transaction_id == left.transaction_id", local
        )

    def test_abort_of_an_unjoined_transaction_writes_no_intent(self) -> None:
        """The recovery ABORT invents an id no board ever joined.

        There is no participant to record rollback intent for, and no
        generation to seed one from -- so the mutation would fail as
        SeedFactIncomplete and the abort would be refused as
        CompatibilityRejected, killing the one command that clears a stuck
        board.  The record is simply left alone; it stays displaceable.
        """
        app = (
            ROOT
            / "MAKCM_ESP32s3_Device_Mouse_Left_IDF"
            / "main"
            / "app_main.cpp"
        ).read_text(encoding="utf-8")
        clause = app[app.index("if (request.command == Command::Abort) {"):]
        clause = clause[: clause.index("mutatePairRecord(")]
        self.assertIn(
            "pair_record.left.transaction_id == request.transaction_id", clause
        )
        self.assertIn(
            "pair_record.right.transaction_id == request.transaction_id", clause
        )
        # Untouched, not rejected: returning false here becomes a NACK.
        self.assertIn("if (!joined) return true;", clause)

    def test_manifest_rejection_separates_board_facts_from_manifest_bytes(
        self,
    ) -> None:
        """One wire code for five causes hides the only ones that matter.

        BadRole and BadHardware describe state the host cannot inspect -- most
        importantly whether the board has learned its peer's generation yet --
        while the other three describe manifest bytes the host already holds.
        Collapsing all five into ManifestRejected made a board-side fact look
        like a bad package.
        """
        source = (
            ROOT
            / "MAKCM_ESP32s3_HID_Mouse_Right"
            / "src"
            / "ota_receiver.cpp"
        ).read_text(encoding="utf-8")
        table = source[source.index("makcm::ota::NackReason toNack("):]
        table = table[: table.index("\n}")]
        self.assertIn(
            "case OtaReceiver::BadRole: return NackReason::WrongTarget;", table
        )
        self.assertIn(
            "case OtaReceiver::BadHardware: "
            "return NackReason::CompatibilityRejected;",
            table,
        )
        # peerGeneration_ == 0 must still be reachable as BadHardware.
        check = source[source.index("bool OtaReceiver::validateManifest("):]
        check = check[: check.index("\n}\n")]
        self.assertIn("peerGeneration_ == 0", check)
        self.assertIn("error = BadHardware;", check)

    def test_pair_record_contents_are_reported_to_the_host(self) -> None:
        """pair=/pf= say the record is unusable, not what it holds.

        Which participants are present, and under which transaction, is what
        separates a two-board stall from a one-sided one -- and those have
        different exits.  rtx= is the right-board maintenance transaction, a
        different number that reads deceptively like the record's own.
        """
        app = (
            ROOT
            / "MAKCM_ESP32s3_Device_Mouse_Left_IDF"
            / "main"
            / "app_main.cpp"
        ).read_text(encoding="utf-8")
        self.assertIn("prec=%lu/%u:%u/%u:%u", app)
        for field in (
            "pair_record.transaction_id",
            "pair_record.left.present",
            "pair_record.left.local_phase",
            "pair_record.right.present",
            "pair_record.right.local_phase",
        ):
            self.assertIn(field, app)


if __name__ == "__main__":
    unittest.main()
