from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class RightOtaBootstrapTests(unittest.TestCase):
    def test_empty_nvs_seeds_the_pending_snapshot_before_first_commit(self) -> None:
        """An erased right board must reach Idle, not FlashWriteFailed.

        ``commitPendingSnapshotV2()`` validates ``pendingSnapshot_.role``.  A
        default-constructed Snapshot has role ``Host``, so the first commit on
        an empty NVS is rejected unless the initialize branch seeds the
        pending copy.  That turned "erase NVS to recover" into the operation
        that bricked the board on every subsequent boot.
        """
        right = (
            ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / "src" /
            "ota_receiver.cpp"
        ).read_text(encoding="utf-8")
        load = right[
            right.index("bool OtaReceiver::loadSnapshotV2") :
            right.index("bool OtaReceiver::partitionMatchesImage")
        ]
        initialize = load[
            load.index("memset(&snapshot_, 0, sizeof(snapshot_));") :
            load.index("return commitPendingSnapshotV2();")
        ]
        self.assertIn("snapshot_.role = makcm::ota::Target::Right;", initialize)
        self.assertIn("pendingSnapshot_ = snapshot_;", initialize)

    def test_embedded_app_version_is_derived_from_the_shared_generation(self) -> None:
        """Both boards must derive `makcm-g<N>` from one macro.

        The right board previously hard-coded its embedded `app_version`
        while its `generation` field came from the shared header, so bumping
        the release left the two disagreeing and no signed package could be
        produced for that generation.
        """
        generation_header = (
            ROOT / "common" / "OtaGeneration.h"
        ).read_text(encoding="utf-8")
        self.assertIn(
            '#define MAKCM_OTA_PAIR_APP_VERSION "makcm-g" '
            "MAKCM_OTA_PAIR_GENERATION_TEXT",
            generation_header,
        )
        right = (
            ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / "src" /
            "ota_receiver.cpp"
        ).read_text(encoding="utf-8")
        left = (
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "components" /
            "makcm_core" / "ota_update_manager.cpp"
        ).read_text(encoding="utf-8")
        for source in (right, left):
            self.assertIn("MAKCM_OTA_PAIR_APP_VERSION", source)
        # No board may pin a literal release version anywhere in firmware.
        for source in (right, left, generation_header):
            self.assertNotRegex(source, r'"makcm-g\d')

    def test_cleanup_wait_is_async_bounded_and_has_explicit_replies(self) -> None:
        source = (
            ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / "src" / "commands.cpp"
        ).read_text(encoding="utf-8")
        header = (
            ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / "include" /
            "EspUsbHost.h"
        ).read_text(encoding="utf-8")
        receive = (
            ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / "src" / "esp_tasks.cpp"
        ).read_text(encoding="utf-8")
        self.assertIn("kOtaBootstrapWaitMs = 3500", header)
        self.assertIn("otaBootstrapPending", source)
        self.assertIn("serviceOtaBootstrap()", source)
        self.assertIn("serviceOtaBootstrap();", receive)
        self.assertNotIn("xSemaphoreTake(cleanupComplete, pdMS_TO_TICKS", source)
        self.assertIn('otaReceiver.sendText("OTA_PENDING\\n")', source)
        self.assertIn('"OTA_READY %lu\\n"', source)
        self.assertIn("otaReceiver.sendText(ready)", source)
        self.assertIn("sendBootstrapRefusal(", source)
        self.assertNotIn('serial1Send("OTA_UNAVAILABLE\\n")', source)

    def test_reservation_release_is_driven_by_host_state_not_the_flag(self) -> None:
        """An orphaned OtaReserved must not be permanent.

        Two bootstrap failure paths -- the lost Empty->OtaReserved CAS and the
        bootstrap-wait timeout -- clear otaHostReserved while leaving hostState
        at OtaReserved.  While the repair was gated on otaHostReserved, those
        paths could never be repaired: every later OTA_MAINT_BEGIN answered
        OTA_UNAVAILABLE and every later controller hot-plug stayed queued, until
        the board was physically power-cycled.
        """
        source = (
            ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / "src" / "commands.cpp"
        ).read_text(encoding="utf-8")
        body = source[source.index("void EspUsbHost::serviceOtaBootstrap()") :]
        release = body[: body.index("const uint32_t transactionId")]
        # The release must be reachable with the flag already false.
        self.assertNotIn("otaHostReserved.load(", release)
        self.assertIn("!otaReceiver.isMaintenanceActive()", release)
        self.assertIn("HostState reserved = HostState::OtaReserved;", release)
        self.assertIn("hostState.compare_exchange_strong(", release)
        self.assertIn("otaHostReserved.store(false", release)
        # Both orphaning paths must still exist, or this regression is untestable.
        self.assertIn(
            "if (!otaHostReserved.compare_exchange_strong(", source)
        self.assertIn("kOtaBootstrapWaitMs", source)

    def test_reservation_release_runs_on_the_command_task(self) -> None:
        """The release CAS is only safe without a lock because one task owns both.

        serviceOtaBootstrap() reads maintenance state and then clears the
        reservation non-atomically.  That is sound only while receiveSerial1
        also owns handleIncomingCommands, so a bootstrap cannot interleave.
        """
        receive = (
            ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / "src" / "esp_tasks.cpp"
        ).read_text(encoding="utf-8")
        fn = receive[receive.index("void EspUsbHost::receiveSerial1") :]
        fn = fn[: fn.index("bool EspUsbHost::serial1Send")]
        self.assertIn("instance->serviceOtaBootstrap();", fn)
        self.assertIn("instance->handleIncomingCommands(commandBuffer, true);", fn)

    def test_maintenance_serial1_keeps_text_bootstrap_audible(self) -> None:
        """NVS-latched maintenance must not silence OTA_MAINT_BEGIN.

        Same shape as the left Idle HELLO gate: a maintenance bool that
        feeds every UART1 byte into MKOT made text bootstrap time out while
        the wire was fine.  Keep MKOT only for mid-frame / magic bytes.
        """
        receive = (
            ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / "src" / "esp_tasks.cpp"
        ).read_text(encoding="utf-8")
        receiver = (
            ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / "include" /
            "OtaReceiver.h"
        ).read_text(encoding="utf-8")
        commands = (
            ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / "src" / "commands.cpp"
        ).read_text(encoding="utf-8")
        self.assertIn("decoderBuffered()", receiver)
        fn = receive[receive.index("void EspUsbHost::receiveSerial1") :]
        # Old blanket gate must stay gone.
        self.assertNotIn(
            "if (otaReceiver.isMaintenanceActive()) {\n"
            "                otaReceiver.feed(byte);\n"
            "                continue;\n"
            "            }",
            fn,
        )
        self.assertIn("otaReceiver.decoderBuffered() != 0", fn)
        self.assertIn("makcm::ota::kMagic[0]", fn)
        # The magic byte only starts MKOT at a text-line boundary.  Testing
        # every byte diverts the 'M' inside "OTA_MAINT_BEGIN" into the MKOT
        # decoder, so the line assembles as "OTA_INT_BEGIN" and is dropped
        # without a reply -- the maintenance-latched board then eats the one
        # command that recovers it and looks like a dead UART.
        self.assertIn(
            "(commandLength == 0 && byte == makcm::ota::kMagic[0])", fn
        )
        self.assertIn("OTA_MAINT_BEGIN ", commands)
        # Relit reserved maintenance must re-bind or explicitly refuse.
        self.assertIn(
            "otaReceiver.isMaintenanceActive() &&\n"
            "            otaHostReserved.load(std::memory_order_acquire))",
            commands,
        )
        # Untrusted Serial0 bootstrap must not go silent.
        self.assertIn(
            "Rejected OTA bootstrap from untrusted command source",
            commands,
        )
        untrusted = commands[
            commands.index("Rejected OTA bootstrap from untrusted")
            - 200 : commands.index("Rejected OTA bootstrap from untrusted")
            + 200
        ]
        self.assertIn("sendBootstrapRefusal(", untrusted)

    def test_production_left_has_no_diagnostic_uart1_probe(self) -> None:
        """The gen16 diagnostic probe must stay out of the production build.

        PROBE_RIGHT_HELLO's answer was inverted (timeout == healthy Idle,
        ok=1 == maintenance-locked), so it could not distinguish a healthy
        right board from a dead UART1.  DEBUG_STATUS is the diagnostic entry
        point; the raw-send / diagnostic-listen escape hatches it needed must
        not survive, because they bypass the exclusive maintenance gate.
        """
        app = (
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "main" /
            "app_main.cpp"
        ).read_text(encoding="utf-8")
        bridge = (
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "components" /
            "makcm_core" / "bridge_transport.cpp"
        ).read_text(encoding="utf-8")
        header = (
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "components" /
            "makcm_core" / "include" / "bridge_transport.h"
        ).read_text(encoding="utf-8")
        for source in (app, bridge, header):
            self.assertNotIn("PROBE_RIGHT_HELLO", source)
            self.assertNotIn("runProbeRightHello", source)
            self.assertNotIn("sendOtaRaw", source)
            self.assertNotIn("DiagnosticOtaListen", source)
            self.assertNotIn("diagnostic_ota_listen_", source)
        # UART1 may only be decoded as MKOT under the one maintenance gate.
        self.assertIn(
            "bool otaRxArmed() const {\n"
            "        return ota_maintenance_.load(std::memory_order_acquire);\n"
            "    }",
            header,
        )

    def test_left_waits_for_ready_before_global_quiesce(self) -> None:
        source = (
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "main" / "app_main.cpp"
        ).read_text(encoding="utf-8")
        bootstrap = source[
            source.index("bool startRightMaintenanceBootstrap(const makcm::ota::Frame &request) {") :
            source.index("bool handleOtaBridgeTextLine(const char *line) {")
        ]
        self.assertNotIn("otaQuiesce", bootstrap)
        ready_start = source.rindex("bool handleOtaBridgeTextLine(const char *line) {")
        ready = source[
            ready_start : source.index("void bridgeOtaFrame", ready_start)
        ]
        self.assertIn("!otaQuiesce(nullptr)", ready)
        self.assertIn("pending_right_request_started_ms", source)
        self.assertIn("ota_config.confirm_timeout_ms = 120000;", source)
        self.assertIn('std::strncmp(line, "OTA_UNAVAILABLE", 15) == 0', ready)

    def test_pending_confirm_rollback_preserves_persistent_evidence(self) -> None:
        manager = (
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "components" /
            "makcm_core" / "ota_update_manager.cpp"
        ).read_text(encoding="utf-8")
        abort = manager[
            manager.index("bool OtaUpdateManager::abort") :
            manager.index("bool OtaUpdateManager::resetDownload")
        ]
        pending_abort = abort[:abort.index("#else")]
        self.assertIn("esp_ota_mark_app_invalid_rollback_and_reboot", pending_abort)
        self.assertNotIn("clearRecord();", pending_abort)

        app = (
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "main" /
            "app_main.cpp"
        ).read_text(encoding="utf-8")
        handler = app[
            app.rindex("void secondaryOtaFrame") :
            app.index("void handleBridgeFrame")
        ]
        self.assertNotIn("clearPairStore();", handler)

    def test_right_prepared_state_is_durable_until_explicit_decision(self) -> None:
        header = (
            ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / "include" /
            "OtaReceiver.h"
        ).read_text(encoding="utf-8")
        self.assertIn("kConfirmTimeoutMs = 120000", header)

        source = (
            ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / "src" /
            "ota_receiver.cpp"
        ).read_text(encoding="utf-8")
        tick = source[source.index("void OtaReceiver::tick()") :]
        self.assertIn("state_ == State::Receiving", tick)
        self.assertNotIn("state_ != State::Idle && state_ != State::Failed", tick)

    def test_local_boot_validation_is_separate_from_pair_commit(self) -> None:
        left_manager = (
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "components" /
            "makcm_core" / "ota_update_manager.cpp"
        ).read_text(encoding="utf-8")
        left_header = (
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "components" /
            "makcm_core" / "include" / "ota_update_manager.h"
        ).read_text(encoding="utf-8")
        left_app = (
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "main" /
            "app_main.cpp"
        ).read_text(encoding="utf-8")
        self.assertIn("local_health_check", left_header)
        self.assertIn("pair_health_check", left_header)
        self.assertIn("bool otaLocalHealth", left_app)
        self.assertIn("ota_boot_prerequisites_ready.load", left_app)
        self.assertNotIn("return secondary.running() && bridge.running();", left_app)
        local_validation = left_manager[
            left_manager.index("bool OtaUpdateManager::validateLocalBoot") :
            left_manager.index("bool OtaUpdateManager::abort")
        ]
        self.assertIn("esp_ota_mark_app_valid_cancel_rollback", local_validation)
        self.assertIn("const bool v2_active = snapshot_.active.present", local_validation)
        self.assertIn("ActivationPending", local_validation)
        self.assertIn("v2_active && !persistLocalValidationFact()", local_validation)
        begin = left_manager[
            left_manager.index("bool OtaUpdateManager::begin") :
            left_manager.index("bool OtaUpdateManager::maintenanceExclusive")
        ]
        # The gen5-to-genN bridge is the per-state dispatch itself.  A V1
        # record must never be intercepted by a blanket fail-closed branch:
        # that would also hide the PendingConfirm resume and the Confirmed
        # tombstone migration below it, so a board that already marked itself
        # valid would enter Fault on its next power-up.
        self.assertIn("gen5-to-genN bridge", begin)
        self.assertNotIn("Any other legacy active record", begin)
        # PENDING_VERIFY first boot with an exact V1 ActivationPending record.
        self.assertLess(
            begin.index("if (pending_verify) {"),
            begin.index("Only an exact VALID state proves"),
        )
        # Reachable after mark-valid: VALID + durable PendingConfirm resumes
        # instead of re-marking or faulting.
        self.assertIn(
            "mark-valid succeeded and its PendingConfirm fact was durable",
            begin,
        )
        self.assertEqual(begin.count("State::PendingConfirm"), 3)
        confirm = left_manager[
            left_manager.index("bool OtaUpdateManager::confirm") :
            left_manager.index("bool OtaUpdateManager::validateLocalBoot")
        ]
        self.assertIn("pair_health_check", confirm)
        self.assertNotIn("esp_ota_mark_app_valid_cancel_rollback", confirm)
        poll = left_manager[left_manager.index("void OtaUpdateManager::poll") :]
        self.assertIn("!local_boot_validated_", poll)
        self.assertNotIn("esp_ota_mark_app_invalid_rollback_and_reboot", poll)
        self.assertIn("setState(makcm::ota::State::Fault", poll)
        self.assertIn("(void)saveRecord();", poll)

        right = (
            ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / "src" /
            "ota_receiver.cpp"
        ).read_text(encoding="utf-8")
        right_confirm = right[
            right.index("bool OtaReceiver::handleConfirm") :
            right.index("bool OtaReceiver::abortUpdate")
        ]
        self.assertIn("validateLocalBoot()", right_confirm)
        self.assertIn("peerCompatible()", right_confirm)
        self.assertIn("healthPassed()", right_confirm)
        right_tick = right[right.index("void OtaReceiver::tick()") :]
        self.assertIn("!localBootValidated_", right_tick)
        self.assertIn("validateLocalBoot();", right_tick)
        handle = left_manager[
            left_manager.index("bool OtaUpdateManager::handle") :
            left_manager.index("bool OtaUpdateManager::enter")
        ]
        self.assertNotIn("validateLocalBoot(now_ms)", handle)
        early_confirm = left_app[
            left_app.index("ota_boot_prerequisites_ready.store") :
            left_app.index("makcm::native::SecondaryConfig")
        ]
        self.assertIn("ota_manager.poll(nowMs())", early_confirm)
        secondary_failure = left_app[
            left_app.index('ESP_LOGE(kTag, "secondary UART startup failed")') :
            left_app.index("makcm::native::BridgeTransportConfig")
        ]
        self.assertNotIn("Command::Abort", secondary_failure)
        self.assertNotIn("clearPairStore()", secondary_failure)
        self.assertIn("ota_quiesced.store(true", secondary_failure)
        protocol = (ROOT / "common" / "OtaProtocol.h").read_text(encoding="utf-8")
        self.assertIn("kStatusLocalBootValidated = 0x0080", protocol)

    def test_locally_validated_abort_refuses_to_guess_previous_slot(self) -> None:
        left = (
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "components" /
            "makcm_core" / "ota_update_manager.cpp"
        ).read_text(encoding="utf-8")
        left_abort = left[
            left.index("bool OtaUpdateManager::abort") :
            left.index("bool OtaUpdateManager::resetDownload")
        ]
        self.assertNotIn("esp_ota_get_next_update_partition", left_abort)
        self.assertIn("RollbackUnavailable", left_abort)

        right = (
            ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / "src" /
            "ota_receiver.cpp"
        ).read_text(encoding="utf-8")
        handle_start = right.index("void OtaReceiver::handleFrame")
        abort_case = right[
            right.index("case Command::Abort:", handle_start) :
            right.index("case Command::Confirm:", handle_start)
        ]
        self.assertIn("localBootValidated_", abort_case)
        self.assertNotIn("esp_ota_get_next_update_partition", abort_case)
        self.assertIn("RollbackUnavailable", abort_case)

    def test_activate_intent_abort_preserves_boot_selection_evidence(self) -> None:
        left = (
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "components" /
            "makcm_core" / "ota_update_manager.cpp"
        ).read_text(encoding="utf-8")
        left_abort = left[
            left.index("bool OtaUpdateManager::abort") :
            left.index("bool OtaUpdateManager::resetDownload")
        ]
        self.assertIn("State::ActivationPending", left_abort)
        self.assertLess(
            left_abort.index("State::ActivationPending"),
            left_abort.index("clearRecord()"),
        )

        right = (
            ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / "src" /
            "ota_receiver.cpp"
        ).read_text(encoding="utf-8")
        handle_start = right.index("void OtaReceiver::handleFrame")
        abort_case = right[
            right.index("case Command::Abort:", handle_start) :
            right.index("case Command::Confirm:", handle_start)
        ]
        self.assertIn("state_ == State::ActivateIntent", abort_case)
        self.assertLess(
            abort_case.index("state_ == State::ActivateIntent"),
            abort_case.index("esp_ota_mark_app_invalid_rollback_and_reboot"),
        )

    def test_right_confirm_keeps_completed_tombstone(self) -> None:
        header = (
            ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / "include" /
            "OtaReceiver.h"
        ).read_text(encoding="utf-8")
        source = (
            ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / "src" /
            "ota_receiver.cpp"
        ).read_text(encoding="utf-8")
        self.assertIn("Confirmed = 6", header)
        confirm = source[
            source.index("bool OtaReceiver::handleConfirm") :
            source.index("bool OtaReceiver::validateLocalBoot")
        ]
        self.assertIn("state_ = State::Confirmed", confirm)
        self.assertIn("persistCommandIntent", confirm)
        self.assertIn("snapshot::Phase::PairPending", confirm)
        self.assertNotIn("prefs_->clear()", confirm)
        load = source[
            source.index("void OtaReceiver::loadPersistentState") :
            source.index("bool OtaReceiver::validateManifest")
        ]
        self.assertIn("State::Confirmed", load)
        self.assertIn("imageState == ESP_OTA_IMG_VALID", load)
        self.assertIn("state_ = State::Idle", load)
        self.assertIn("completedMatches(frame.transaction_id)", source)
        self.assertIn('kCompletedNamespace[] = "makcm_ota_done"', source)

    def test_left_completed_tombstone_is_separate_from_active_transaction(self) -> None:
        source = (
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "components" /
            "makcm_core" / "ota_update_manager.cpp"
        ).read_text(encoding="utf-8")
        header = (
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "components" /
            "makcm_core" / "include" / "ota_update_manager.h"
        ).read_text(encoding="utf-8")
        self.assertIn('kNvsCompletedRecord[] = "local_done"', source)
        self.assertIn("completed_record_valid_", header)
        confirm = source[
            source.index("bool OtaUpdateManager::confirm") :
            source.index("bool OtaUpdateManager::validateLocalBoot")
        ]
        self.assertLess(
            confirm.index("saveCompletedRecord()"),
            confirm.index("clearRecord()"),
        )
        replay = source[
            source.index("OtaUpdateManager::ReplayDecision OtaUpdateManager::replayCommand") :
            source.index("bool OtaUpdateManager::resumeIncompleteCommand")
        ]
        self.assertIn("snapshot_.completed.present", replay)
        self.assertIn("terminal_request_sha256", replay)
        self.assertIn("fillTombstoneResponse", replay)
        fill = source[
            source.index("void OtaUpdateManager::fillResponse") :
            source.index("bool OtaUpdateManager::handle")
        ]
        self.assertNotIn("completed_response", fill)
        finalize = source[
            source.index("bool OtaUpdateManager::finalizeCommandResponse") :
            source.index("bool OtaUpdateManager::saveRecord")
        ]
        self.assertIn("next.completed", finalize)
        self.assertIn("std::memset(&next.active", finalize)

    def test_right_activate_is_intent_first_and_prepared_is_recoverable(self) -> None:
        header = (
            ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / "include" /
            "OtaReceiver.h"
        ).read_text(encoding="utf-8")
        source = (
            ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / "src" /
            "ota_receiver.cpp"
        ).read_text(encoding="utf-8")
        self.assertIn("ActivateIntent = 7", header)
        activate = source[
            source.index("bool OtaReceiver::handleActivate") :
            source.index("bool OtaReceiver::handleConfirm")
        ]
        self.assertLess(
            activate.index("state_ = State::ActivateIntent"),
            activate.index("esp_ota_set_boot_partition"),
        )
        self.assertLess(
            activate.index("persistCommandIntent"),
            activate.index("esp_ota_set_boot_partition"),
        )
        self.assertIn("esp_ota_get_boot_partition() != targetPartition_", activate)
        self.assertIn("state_ = State::Activated", activate)

        persist = source[
            source.index("bool OtaReceiver::persistState") :
            source.index("void OtaReceiver::loadPersistentState")
        ]
        self.assertNotIn('putUChar("slot", targetSubtype_)', persist)
        self.assertIn("persistFactOrFail", activate)
        load = source[
            source.index("void OtaReceiver::loadPersistentState") :
            source.index("bool OtaReceiver::validateManifest")
        ]
        self.assertIn("loadSnapshotV2()", source)
        self.assertIn("restoreFromSnapshotV2", load)
        self.assertIn("snapshot_.active.present", load)

    def test_main_task_stack_fix_removes_large_automatic_frames(self) -> None:
        app = (
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "main" /
            "app_main.cpp"
        ).read_text(encoding="utf-8")
        failure = app[
            app.index('ESP_LOGE(kTag, "secondary UART startup failed")') :
            app.index("makcm::native::BridgeTransportConfig")
        ]
        self.assertNotIn("makcm::ota::Frame request{}", failure)
        self.assertNotIn("makcm::ota::Frame response{}", failure)
        self.assertNotIn("ota_control_response = {};", app)
        self.assertNotIn("pending_right_request = {};", app)
        protocol = (ROOT / "common" / "OtaProtocol.h").read_text(encoding="utf-8")
        self.assertNotIn("frame = {};", protocol)
        self.assertIn("uxTaskGetStackHighWaterMark(nullptr)", app)
        self.assertIn("mstack=%lu", app)
        main_cmake = (
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "main" /
            "CMakeLists.txt"
        ).read_text(encoding="utf-8")
        core_cmake = (
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "components" /
            "makcm_core" / "CMakeLists.txt"
        ).read_text(encoding="utf-8")
        self.assertIn("-fstack-usage", main_cmake)
        self.assertIn("-fstack-usage", core_cmake)
        right_platformio = (
            ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / "platformio.ini"
        ).read_text(encoding="utf-8")
        self.assertIn("-fstack-usage", right_platformio)
        self.assertIn("stick_status_response", app)

        sdkconfig = (
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "sdkconfig"
        ).read_text(encoding="utf-8")
        defaults = (
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" /
            "sdkconfig.defaults"
        ).read_text(encoding="utf-8")
        self.assertIn("CONFIG_ESP_MAIN_TASK_STACK_SIZE=12288", sdkconfig)
        self.assertIn("CONFIG_ESP_MAIN_TASK_STACK_SIZE=12288", defaults)

    def test_activation_reboots_restore_pair_confirm_channel(self) -> None:
        right = (
            ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / "src" /
            "ota_receiver.cpp"
        ).read_text(encoding="utf-8")
        self.assertIn("loadSnapshotV2", right)
        self.assertIn("maintenanceActive_ = true", right)

        left = (
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "main" /
            "app_main.cpp"
        ).read_text(encoding="utf-8")
        self.assertIn("pairRecordRequiresHostReconcile", left)
        self.assertIn("bridge.setOtaMaintenance(true)", left)

        manager = (
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "components" /
            "makcm_core" / "ota_update_manager.cpp"
        ).read_text(encoding="utf-8")
        confirm = manager[
            manager.index("bool OtaUpdateManager::confirm") :
            manager.index("bool OtaUpdateManager::abort")
        ]
        self.assertIn("restart_requested_.store(true", confirm)

    def test_right_activate_keeps_binary_channel_until_left_reboots(self) -> None:
        source = (
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "main" /
            "app_main.cpp"
        ).read_text(encoding="utf-8")
        bridge_start = source.rindex("void bridgeOtaFrame")
        bridge_response = source[
            bridge_start : source.index("void secondaryOtaFrame", bridge_start)
        ]
        reset_condition = bridge_response[
            bridge_response.index("const bool idle_maintenance_status") :
            bridge_response.index("right_link_reset_at_ms.store")
        ]
        # The bridge may only be released from exact Status V2 evidence: an
        # idle/no-transaction maintenance probe, or the pair-committed
        # tombstone.  ACTIVATE never releases it, because the right board
        # reboots into AwaitConfirm while still owning binary UART1.
        self.assertIn("idle_maintenance_status", reset_condition)
        self.assertIn("exact_pair_completed", reset_condition)
        self.assertNotIn("Command::Activate", reset_condition)
        # A V1 ACK carries no transaction/image identity and must never be
        # accepted as terminal or release evidence.
        self.assertNotIn("valid_status_v1", reset_condition)

    def test_idle_status_probe_releases_both_maintenance_ends(self) -> None:
        right = (
            ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / "src" /
            "ota_receiver.cpp"
        ).read_text(encoding="utf-8")
        handle_start = right.index("void OtaReceiver::handleFrame")
        status_case = right[
            right.index("case Command::Status:", handle_start) :
            right.index("case Command::Activate:", handle_start)
        ]
        self.assertIn("state_ == State::Idle", status_case)
        self.assertIn("transactionId_ == 0", status_case)
        self.assertIn("maintenanceActive_ = false", status_case)

        left = (
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "main" /
            "app_main.cpp"
        ).read_text(encoding="utf-8")
        bridge_start = left.rindex("void bridgeOtaFrame")
        bridge = left[
            bridge_start : left.index("void secondaryOtaFrame", bridge_start)
        ]
        self.assertIn("idle_maintenance_status", bridge)
        self.assertIn("status_v2.target_generation == 0", bridge)
        self.assertIn("kStatusPendingVerify", bridge)
        release = left[
            left.index("bool tryReleaseOtaQuiesce()") :
            left.index("bool otaPairHealth")
        ]
        self.assertIn("exact_unpaired_idle", release)
        self.assertIn("ota_unpaired_idle_release_authorized", release)

    def test_empty_maintenance_abort_needs_no_image_ledger(self) -> None:
        manager = (
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "components" /
            "makcm_core" / "ota_update_manager.cpp"
        ).read_text(encoding="utf-8")
        abort = manager[
            manager.index("bool OtaUpdateManager::abort") :
            manager.index("bool OtaUpdateManager::resetDownload")
        ]
        empty = abort[:abort.index("State::ActivationPending")]
        self.assertIn("State::Maintenance", empty)
        self.assertIn("!snapshot_.active.present", empty)
        self.assertIn("setState(makcm::ota::State::Idle", empty)
        self.assertNotIn("persistCommandIntent", empty)

        handle = manager[
            manager.index("bool OtaUpdateManager::handle") :
            manager.index("bool OtaUpdateManager::enter")
        ]
        self.assertIn("empty_maintenance_abort", handle)
        self.assertIn("!(empty_maintenance_abort && ok)", handle)

        right = (
            ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / "src" /
            "ota_receiver.cpp"
        ).read_text(encoding="utf-8")
        frame_start = right.index("void OtaReceiver::handleFrame")
        abort_case = right[
            right.index("case Command::Abort:", frame_start) :
            right.index("case Command::Confirm:", frame_start)
        ]
        self.assertIn("state_ == State::Idle && maintenanceActive_", abort_case)
        self.assertIn("!snapshot_.active.present", abort_case)
        self.assertIn("releaseMaintenanceAfterResponse = true", abort_case)
        post_response = right[
            right.index("emit(responseFrame_);", right.index("void OtaReceiver::handleFrame")) :
            right.index("if (restart)", right.index("void OtaReceiver::handleFrame"))
        ]
        self.assertIn("maintenanceActive_.store(false", post_response)

    def test_left_release_requires_completed_pair_and_retains_tombstone(self) -> None:
        left = (
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "main" /
            "app_main.cpp"
        ).read_text(encoding="utf-8")
        start = left.index("bool tryReleaseOtaQuiesce()")
        release = left[start:left.index("bool otaPairHealth", start)]
        self.assertIn("ota_quiesced.load", release)
        self.assertIn("ota_manager.maintenanceExclusive()", release)
        self.assertIn("pair_record_corrupt", release)
        self.assertIn("pairRecordHasExactPairCommit", release)
        self.assertNotIn("clearPairStore()", release)
        self.assertIn("bridge.setOtaMaintenance(false)", release)
        self.assertIn("secondary.setOtaMaintenance(false)", release)
        self.assertIn("std::memset(&pending_right_request", release)
        self.assertIn("pending_right_request_valid = false", release)
        self.assertIn("right_maintenance_transaction = 0", release)
        self.assertIn("active_session.store(0", release)
        self.assertIn("pending_session.store(0", release)
        self.assertIn("accepted_session.store(0", release)
        self.assertIn("ota_quiesced.store(false", release)
        self.assertIn("ProxyState::WaitingProfile", release)

    def test_left_confirm_refreshes_post_reboot_local_fact_before_pair_intent(self) -> None:
        left = (
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "main" /
            "app_main.cpp"
        ).read_text(encoding="utf-8")
        start = left.index("bool preparePairIntentForRequest")
        end = left.index("bool updatePairFromLocalStatus", start)
        prepare = left[start:end]
        confirm = prepare[
            prepare.index("if (request.command == Command::Confirm)") :
            prepare.index("if (request.command == Command::Abort)")
        ]
        self.assertIn("requestLocalStatusV2(request, local_status)", confirm)
        self.assertIn(
            "pairFactFromStatus(local_status, Target::Left, local_fact)",
            confirm,
        )
        self.assertIn("local_fact.local_validated", confirm)
        self.assertIn("pair_record.right.pair_committed", confirm)
        self.assertIn(
            "mutatePairRecord(request.transaction_id,\n"
            "                                PairStage::PairCommitIntent",
            confirm,
        )
        self.assertIn("&local_fact,\n                                &pair_record.right", confirm)

    def test_internal_left_status_probe_targets_left_participant(self) -> None:
        left = (
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "main" /
            "app_main.cpp"
        ).read_text(encoding="utf-8")
        start = left.index("bool requestLocalStatusV2")
        end = left.index("bool pairReadyForLeftConfirm", start)
        probe = left[start:end]
        self.assertIn(
            "pair_status_probe_request.target = makcm::ota::Target::Left;",
            probe,
        )
        self.assertNotIn(
            "pair_status_probe_request.target = makcm::ota::Target::Host;",
            probe,
        )

        reset_start = left.index("if (reset_at != 0")
        reset_end = left.index("if (pending_right_request_valid", reset_start)
        reset = left[reset_start:reset_end]
        self.assertIn("tryReleaseOtaQuiesce()", reset)
        self.assertNotIn("bridge.setOtaMaintenance(false);", reset)
        self.assertNotIn("secondary.setOtaMaintenance(false);", reset)

    def test_failed_bootstrap_without_global_quiesce_cannot_release(self) -> None:
        left = (
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "main" /
            "app_main.cpp"
        ).read_text(encoding="utf-8")
        start = left.index("bool tryReleaseOtaQuiesce()")
        release = left[start:left.index("bool otaPairHealth", start)]
        # The first guard must gate all maintenance release work on the
        # global quiesce latch, not merely on right_link_reset_at_ms.
        self.assertLess(
            release.index("ota_quiesced.load"),
            release.index("bridge.setOtaMaintenance(false)"),
        )

    def test_right_status_header_matches_payload_resume_offset(self) -> None:
        right = (
            ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / "src" /
            "ota_receiver.cpp"
        ).read_text(encoding="utf-8")
        status = right[
            right.index("void OtaReceiver::sendStatus(") :
            right.index("bool OtaReceiver::persistState()")
        ]
        self.assertIn("responseFrame_.offset = bytesWritten_;", status)
        self.assertIn("status.next_offset = bytesWritten_;", status)
        self.assertIn(
            "responseFrame_.sequence = replySequence_ == 0 ? 1 : replySequence_;",
            status,
        )

    def test_left_relays_correlated_right_response_verbatim(self) -> None:
        """The bridge correlates by tx+seq, then relays; it never vetoes.

        The old path validated the decoded payload and, on any mismatch,
        latched a pair fault (which quiesced the bridge with no exit) and
        replaced the peer's answer with a manufactured V1 NACK -- which the
        host's Status-V2 filter drops, so every right-board response read as
        a timeout.  In host-driven OTA the host owns response interpretation;
        the left may only correlate, cache diagnostics, and forward.
        """
        left = (
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "main" /
            "app_main.cpp"
        ).read_text(encoding="utf-8")
        bridge_start = left.rindex(
            "void bridgeOtaFrame(void *, const makcm::ota::Frame &frame)"
        )
        bridge = left[
            bridge_start : left.index("void secondaryOtaFrame", bridge_start)
        ]
        for evidence in (
            "right_response_expected",
            "frame.transaction_id == right_expected_transaction",
            "frame.sequence == right_expected_sequence",
        ):
            self.assertIn(evidence, bridge)
        self.assertIn("const bool valid_status_v2", bridge)
        # The peer's frame reaches the host unmodified.
        self.assertIn("secondary.sendOtaFrame(frame);", bridge)
        # No veto, no substitute NACK, no fault latch on this path.
        self.assertNotIn("latchPairFault", bridge)
        self.assertNotIn("sendOtaNack", bridge)
        self.assertNotIn("fillOtaControlResponse", bridge)
        self.assertLess(
            bridge.index("const bool exact_response"),
            bridge.index("right_status_flags.store"),
        )
        # The observation layer the veto belonged to is gone entirely.
        self.assertNotIn("observeRightOtaResponse", left)

    def test_status_v2_correlation_is_not_overwritten_by_durable_command(self) -> None:
        left_manager = (
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "components" /
            "makcm_core" / "ota_update_manager.cpp"
        ).read_text(encoding="utf-8")
        left_fill = left_manager[
            left_manager.index("bool OtaUpdateManager::fillStatusV2") :
            left_manager.index("bool OtaUpdateManager::handle")
        ]
        right = (
            ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / "src" /
            "ota_receiver.cpp"
        ).read_text(encoding="utf-8")
        right_fill = right[
            right.index("bool OtaReceiver::fillStatusV2") :
            right.index("void OtaReceiver::sendStatus")
        ]
        for fill in (left_fill, right_fill):
            self.assertIn("status.last_command = request.command;", fill)
            self.assertIn(
                "status.last_response_command = Command::StatusResponse;", fill
            )
            self.assertNotIn("status.last_command = active", fill)
            self.assertNotIn("active.ledger[index].result", fill)
            self.assertNotIn("active->ledger[index].result", fill)

    def test_left_fault_state_cannot_claim_local_boot_health(self) -> None:
        left = (
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "main" /
            "app_main.cpp"
        ).read_text(encoding="utf-8")
        health = left[
            left.index("bool otaLocalHealth(void *)") :
            left.index("bool forwardRightOtaRequest")
        ]
        self.assertIn("State::Fault", health)

    def test_right_local_health_does_not_require_controller_enumeration(self) -> None:
        header = (
            ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / "include" /
            "OtaReceiver.h"
        ).read_text(encoding="utf-8")
        tasks = (
            ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / "src" /
            "esp_tasks.cpp"
        ).read_text(encoding="utf-8")
        receiver = (
            ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / "src" /
            "ota_receiver.cpp"
        ).read_text(encoding="utf-8")
        self.assertIn("LocalHealthCheck", header)
        self.assertIn("PairHealthCheck", header)
        local_lambda = tasks[tasks.index("otaReceiver.begin(") :]
        self.assertLess(
            local_lambda.index("cleanupComplete != nullptr"),
            local_lambda.index("hostInstallStatus == ESP_OK"),
        )
        validate = receiver[
            receiver.index("bool OtaReceiver::validateLocalBoot") :
            receiver.index("bool OtaReceiver::abortUpdate")
        ]
        self.assertIn("localHealthPassed()", validate)
        self.assertNotIn("healthPassed()", validate)

    def test_right_rollback_intent_recovers_from_exact_previous_slot(self) -> None:
        receiver = (
            ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / "src" /
            "ota_receiver.cpp"
        ).read_text(encoding="utf-8")
        restore = receiver[
            receiver.index("bool OtaReceiver::restoreFromSnapshotV2") :
            receiver.index("bool OtaReceiver::buildActiveIdentity")
        ]
        self.assertIn("active.phase == Phase::RollbackIntent", restore)
        self.assertIn("runningPrevious", restore)
        self.assertIn("bootPrevious", restore)
        self.assertIn("partitionMatchesPreviousImage", restore)
        self.assertIn("entry.effect_phase = EffectPhase::ExternalEffectObserved", restore)
        self.assertIn("next.active.phase = Phase::RolledBack", restore)

    def test_right_abort_failure_cannot_release_maintenance(self) -> None:
        receiver = (
            ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / "src" /
            "ota_receiver.cpp"
        ).read_text(encoding="utf-8")
        handle = receiver[
            receiver.index("void OtaReceiver::handleFrame") :
            receiver.index("void OtaReceiver::feed")
        ]
        self.assertIn("if (!abortUpdate(Ok, false))", handle)
        self.assertIn("releaseMaintenanceAfterResponse = false;", handle)
        self.assertIn("restart = false;", handle)
        self.assertIn("releaseDurableTerminal()", handle)

    def test_right_ota_initialization_failure_reserves_data_plane(self) -> None:
        receiver = (
            ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / "src" /
            "ota_receiver.cpp"
        ).read_text(encoding="utf-8")
        tasks = (
            ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / "src" /
            "esp_tasks.cpp"
        ).read_text(encoding="utf-8")
        begin = receiver[
            receiver.index("void OtaReceiver::begin") :
            receiver.index("bool OtaReceiver::enterMaintenance")
        ]
        self.assertGreaterEqual(begin.count("maintenanceActive_.store(true"), 3)
        self.assertIn(
            "otaHostReserved.store(otaReceiver.isMaintenanceActive()", tasks
        )

    def test_data_replay_binds_sequence_and_sha256_on_both_boards(self) -> None:
        right = (
            ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / "src" /
            "ota_receiver.cpp"
        ).read_text(encoding="utf-8")
        left = (
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "components" /
            "makcm_core" / "ota_update_manager.cpp"
        ).read_text(encoding="utf-8")
        for source, sequence, digest in (
            (right, "sequence == lastDataSequence_", "lastDataSha256_"),
            (left, "request.sequence == last_data_sequence_", "last_data_sha256_"),
        ):
            self.assertIn(sequence, source)
            self.assertIn(digest, source)
            self.assertIn("sha256", source.lower())

    def test_every_mkot_ring_holds_a_whole_data_frame(self) -> None:
        # A UART ring smaller than kMaxWireSize cannot hold one DATA frame, so
        # reception depends on the RX task being scheduled mid-frame instead of
        # being structurally guaranteed.  The dropped byte fails CRC, the board
        # answers nothing, and the host sees only an unexplained timeout -- the
        # right board's ring was 4096 against a 4124-byte frame, which let every
        # MANIFEST through and no DATA at all.
        protocol = (ROOT / "common" / "OtaProtocol.h").read_text(encoding="utf-8")
        self.assertIn("kHeaderSize = 24", protocol)
        self.assertIn("kMaxChunkSize = 4096", protocol)
        self.assertIn("kCrcSize = 4", protocol)
        max_wire = 24 + 4096 + 4

        rings = (
            (
                ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / "src" / "main.cpp",
                "Serial1.setRxBufferSize(",
            ),
            (
                ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "components" /
                "makcm_core" / "include" / "secondary.h",
                "int rx_buffer_size = ",
            ),
        )
        for path, anchor in rings:
            source = path.read_text(encoding="utf-8")
            line = source[source.index(anchor):]
            line = line[: line.index("\n")]
            # Derived from the protocol constant, never a magic number: a chunk
            # size change must not silently outgrow the ring again.
            self.assertIn("makcm::ota::kMaxWireSize", line, path.name)
            self.assertNotIn("4096", line, path.name)

        # The left's UART1 bridge ring is configured separately; it must clear
        # the same bar.
        app = (
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "main" /
            "app_main.cpp"
        ).read_text(encoding="utf-8")
        bridge_rx = app[app.index("bridge_config.rx_buffer_size = "):]
        bridge_rx = int(bridge_rx[len("bridge_config.rx_buffer_size = "):
                                  bridge_rx.index(";")])
        self.assertGreaterEqual(bridge_rx, max_wire)

    def test_usb_hotplug_and_ota_bootstrap_share_atomic_empty_claim(self) -> None:
        header = (
            ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / "include" /
            "EspUsbHost.h"
        ).read_text(encoding="utf-8")
        commands = (
            ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / "src" /
            "commands.cpp"
        ).read_text(encoding="utf-8")
        host = (
            ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / "src" /
            "esp_usb_host.cpp"
        ).read_text(encoding="utf-8")
        self.assertIn("OtaReserved", header)
        self.assertIn("std::atomic<HostState> hostState", header)
        self.assertIn("HostState::OtaReserved", commands)
        self.assertIn("compare_exchange_strong", commands)
        callback = host[host.index("void EspUsbHost::_clientEventCallback") :]
        self.assertIn("HostState::Opening", callback)
        self.assertIn("compare_exchange_strong", callback)

    def _right_commands(self) -> str:
        return (
            ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / "src" / "commands.cpp"
        ).read_text(encoding="utf-8")

    def test_host_fault_does_not_need_a_power_cycle(self) -> None:
        """Fault is written in one place and cleared in none.

        It means the USB *data plane* is untrustworthy, but OTA's first act is
        to tear USB down entirely -- so refusing the bootstrap blocked the one
        operation that does not depend on what broke, using the very failure it
        would repair.  A failed cleanup is also where an in-place teardown
        cannot be trusted, so the exit is the teardown that always works.
        """
        host = (
            ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / "src" / "esp_usb_host.cpp"
        ).read_text(encoding="utf-8")
        commands = self._right_commands()
        # Still exactly one writer, and still nothing that clears it in place.
        self.assertEqual(host.count("hostState = HostState::Fault;"), 1)
        # Both bootstrap paths must restart out of Fault, not refuse.
        for fn in ("handleIncomingCommands", "serviceOtaBootstrap"):
            body = commands[commands.index(f"EspUsbHost::{fn}("):]
            body = body[: body.index("\n}\n")]
            fault = body.index("HostState::Fault")
            after = body[fault:]
            self.assertIn("esp_restart();", after)
            # OTA_PENDING is the bridge's keep-retrying marker, so the host's
            # existing retry loop rides through the reboot.
            self.assertIn('sendText("OTA_PENDING\\n")', after)
            restart_at = after.index("esp_restart();")
            self.assertNotIn(
                "OTA_UNAVAILABLE", after[:restart_at],
                f"{fn} still refuses before restarting out of Fault",
            )

    def test_every_bootstrap_refusal_names_its_site(self) -> None:
        """Eight refusals shared one bare OTA_UNAVAILABLE line.

        "busy with a device" and "can never accept a bootstrap again" reached
        the host as the same NotInMaintenance NACK.
        """
        commands = self._right_commands()
        self.assertNotIn('sendText("OTA_UNAVAILABLE\\n")', commands)
        self.assertIn('"OTA_UNAVAILABLE %u %u %u\\n"', commands)
        enum = commands[commands.index("enum class BootstrapRefusal"):]
        enum = enum[: enum.index("};")]
        sites = [
            "UntrustedSource", "MalformedRequest", "PendingOtherTransaction",
            "RebindRejected", "ReservationLost", "MaintenanceRejected",
            "HostStateUnavailable", "BootstrapWaitTimeout",
        ]
        for site in sites:
            self.assertIn(site, enum)
            self.assertIn(f"BootstrapRefusal::{site}", commands)
        # A timeout and a fail-closed host are different facts.
        service = commands[commands.index("EspUsbHost::serviceOtaBootstrap("):]
        self.assertNotIn("HostState::Fault ||\n        timedOut", service)

    def test_left_forwards_the_attributed_refusal(self) -> None:
        """The NACK the host receives cannot carry the site; the diag must."""
        app = (
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "main" /
            "app_main.cpp"
        ).read_text(encoding="utf-8")
        # Exact strcmp would silently stop matching the attributed line.
        self.assertNotIn('std::strcmp(line, "OTA_UNAVAILABLE")', app)
        self.assertIn('std::strncmp(line, "OTA_UNAVAILABLE", 15)', app)
        self.assertIn('"OTA_UNAVAILABLE %u %u %u"', app)
        self.assertIn("right_bootstrap_refusal_site", app)
        self.assertIn("rrefuse=%u/%u/%u", app)


    def test_a_dead_transaction_cannot_own_the_right_board_forever(self) -> None:
        """loadPersistentState restores transactionId_ from the active snapshot.

        Both enterMaintenance guards then rejected every transaction that was
        not that one, with no displacement rule anywhere -- unlike the left
        board, which at least had escape predicates.  One interrupted download
        made the board refuse all future updates until a BOOT cable.
        """
        source = (
            ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / "src" / "ota_receiver.cpp"
        ).read_text(encoding="utf-8")
        predicate = source[
            source.index("bool OtaReceiver::activeSnapshotIsDisplaceable()"):
        ]
        predicate = predicate[: predicate.index("\n}\n")]
        # boot != target is the whole safety claim, exactly as on the left: an
        # image the bootloader will not select installed nothing.
        self.assertIn("esp_ota_get_boot_partition() != target", predicate)
        body = source[source.index("bool OtaReceiver::enterMaintenance("):]
        body = body[: body.index("\n}\n")]
        for guard in ("ActiveTransactionMismatch", "BoundTransactionMismatch"):
            at = body.index(f"MaintenanceReject::{guard}")
            self.assertIn("displaceable", body[:at])
        # Accepting a displaced transaction must clear the id it displaced.
        self.assertIn("transactionId_ = 0;", body)
        # Every rejection names its site; the refusal line is the only channel,
        # since a board out of maintenance cannot answer Status V2 either.
        for site in ("NotInitialized", "ZeroTransaction",
                     "ActiveTransactionMismatch", "BoundTransactionMismatch",
                     "BootstrapTransactionMismatch", "SerialBusy"):
            self.assertIn(f"MaintenanceReject::{site}", body)


if __name__ == "__main__":
    unittest.main()
