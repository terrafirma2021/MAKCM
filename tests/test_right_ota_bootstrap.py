from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class RightOtaBootstrapTests(unittest.TestCase):
    def test_right_boot_state_comes_only_from_running_partition(self) -> None:
        right = (
            ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / "src" /
            "ota_receiver.cpp"
        ).read_text(encoding="utf-8")
        begin = right[
            right.index("void OtaReceiver::begin") :
            right.index("bool OtaReceiver::enterMaintenance")
        ]
        self.assertIn("esp_ota_get_state_partition", begin)
        self.assertIn("ESP_OTA_IMG_PENDING_VERIFY", begin)
        self.assertNotIn("Preferences", right)
        self.assertNotIn("Snapshot", right)

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
        # Native OTA hunts the full MKOT magic at every position. A failed
        # prefix is spilled back into the text buffer, so OTA_MAINT_BEGIN stays
        # audible without imposing the retired line-boundary decoder rule.
        self.assertIn("byte == makcm::ota::kMagic[otaMagicRun]", fn)
        self.assertIn("if (otaMagicRun != 0)", fn)
        self.assertIn("otaMagicPending[i]", fn)
        self.assertNotIn(
            "commandLength == 0 && byte == makcm::ota::kMagic[0]", fn
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

    def test_pending_confirm_rollback_is_deferred_until_after_response(self) -> None:
        manager = (
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "components" /
            "makcm_core" / "ota_update_manager.cpp"
        ).read_text(encoding="utf-8")
        abort = manager[
            manager.index("bool OtaUpdateManager::abort") :
            manager.index("bool OtaUpdateManager::resetDownload")
        ]
        pending_abort = abort[:abort.index("#else")]
        self.assertIn("rollback_requested_.store(true", pending_abort)
        self.assertNotIn("esp_ota_mark_app_invalid_rollback_and_reboot", pending_abort)
        restart = manager[
            manager.index("bool OtaUpdateManager::takeRestartRequest") :
            manager.index("uint32_t OtaUpdateManager::statusFlags")
        ]
        self.assertIn("rollback_requested_.exchange(false", restart)
        self.assertIn("esp_ota_mark_app_invalid_rollback_and_reboot", restart)

    def test_right_transient_timeout_releases_maintenance(self) -> None:
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
        self.assertIn("state_ == State::Prepared", tick)
        self.assertIn("releaseMaintenance();", tick)

    def test_left_confirm_uses_native_boot_state_and_both_health_checks(self) -> None:
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
        self.assertIn("peer_health_check", left_header)
        self.assertIn("bool otaLocalHealth", left_app)
        self.assertIn("ota_boot_prerequisites_ready.load", left_app)
        self.assertNotIn("return secondary.running() && bridge.running();", left_app)
        begin = left_manager[
            left_manager.index("bool OtaUpdateManager::begin") :
            left_manager.index("bool OtaUpdateManager::takeRestartRequest")
        ]
        self.assertIn("esp_ota_get_state_partition", begin)
        self.assertIn("ESP_OTA_IMG_PENDING_VERIFY", begin)
        self.assertIn("makcm::ota::State::PendingConfirm", begin)
        self.assertNotIn("Snapshot", left_manager)
        self.assertNotIn("Preferences", left_manager)
        confirm = left_manager[
            left_manager.index("bool OtaUpdateManager::confirm") :
            left_manager.index("bool OtaUpdateManager::abort")
        ]
        self.assertIn("local_health_check", confirm)
        self.assertIn("peer_health_check", confirm)
        self.assertIn("esp_ota_mark_app_valid_cancel_rollback", confirm)
        self.assertIn("makcm::ota::State::Idle", confirm)

        right = (
            ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / "src" /
            "ota_receiver.cpp"
        ).read_text(encoding="utf-8")
        right_confirm = right[
            right.index("bool OtaReceiver::handleConfirm") :
            right.index("void OtaReceiver::handleFrame")
        ]
        self.assertIn("localHealthPassed()", right_confirm)
        self.assertIn("peerCompatible()", right_confirm)
        self.assertIn("healthPassed()", right_confirm)
        right_tick = right[right.index("void OtaReceiver::tick()") :]
        self.assertIn("State::PendingConfirm", right_tick)
        self.assertIn("esp_ota_mark_app_invalid_rollback_and_reboot", right_tick)
        handle = left_manager[
            left_manager.index("bool OtaUpdateManager::handle") :
            left_manager.index("bool OtaUpdateManager::enter")
        ]
        self.assertNotIn("validateLocalBoot", handle)
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
        self.assertIn("ota_quiesced.store(true", secondary_failure)
        protocol = (ROOT / "common" / "OtaProtocol.h").read_text(encoding="utf-8")
        self.assertIn("kStatusLocalBootValidated = 0x0020", protocol)

    def test_pending_confirm_abort_uses_bootloader_rollback(self) -> None:
        right = (
            ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / "src" /
            "ota_receiver.cpp"
        ).read_text(encoding="utf-8")
        handle_start = right.index("void OtaReceiver::handleFrame")
        abort_case = right[
            right.index("case Command::Abort:", handle_start) :
            right.index("case Command::Confirm:", handle_start)
        ]
        self.assertNotIn("esp_ota_get_next_update_partition", abort_case)
        self.assertIn("state_ == State::PendingConfirm", abort_case)
        self.assertIn("rollbackAfterResponse = true", abort_case)

    def test_right_has_no_activation_intent_state(self) -> None:
        right = (
            ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / "src" /
            "ota_receiver.cpp"
        ).read_text(encoding="utf-8")
        self.assertNotIn("ActivateIntent", right)
        self.assertNotIn("ActivationPending", right)

    def test_right_confirm_marks_valid_then_returns_to_idle(self) -> None:
        header = (
            ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / "include" /
            "OtaReceiver.h"
        ).read_text(encoding="utf-8")
        source = (
            ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / "src" /
            "ota_receiver.cpp"
        ).read_text(encoding="utf-8")
        self.assertIn("using State = makcm::ota::State;", header)
        confirm = source[
            source.index("bool OtaReceiver::handleConfirm") :
            source.index("void OtaReceiver::handleFrame")
        ]
        self.assertIn("esp_ota_mark_app_valid_cancel_rollback", confirm)
        self.assertIn("state_ = State::Idle", confirm)
        self.assertNotIn("Preferences", source)
        self.assertNotIn("tombstone", source)

    def test_left_has_no_persistent_transaction_compatibility_layer(self) -> None:
        source = (
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "components" /
            "makcm_core" / "ota_update_manager.cpp"
        ).read_text(encoding="utf-8")
        for retired in (
            "Preferences", "Snapshot", "CompletedRecord", "tombstone",
            "saveRecord", "loadRecord", "replayCommand",
        ):
            self.assertNotIn(retired, source)

    def test_right_activate_sets_boot_then_acks_and_restarts(self) -> None:
        header = (
            ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / "include" /
            "OtaReceiver.h"
        ).read_text(encoding="utf-8")
        source = (
            ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / "src" /
            "ota_receiver.cpp"
        ).read_text(encoding="utf-8")
        self.assertNotIn("enum class State", header)
        activate = source[
            source.index("bool OtaReceiver::handleActivate") :
            source.index("bool OtaReceiver::handleConfirm")
        ]
        self.assertIn("esp_ota_set_boot_partition", activate)
        self.assertIn("esp_ota_get_boot_partition() != targetPartition_", activate)
        handle = source[source.index("void OtaReceiver::handleFrame") :]
        self.assertLess(handle.index("emitResponse"), handle.index("esp_restart()"))
        self.assertNotIn("persist", activate)

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
        right_cmake = (
            ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / "main" /
            "CMakeLists.txt"
        ).read_text(encoding="utf-8")
        self.assertIn("target_compile_options", right_cmake)
        self.assertIn("-fstack-usage", right_cmake)
        self.assertIn("stick_status_response", app)

        defaults = (
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" /
            "sdkconfig.defaults"
        ).read_text(encoding="utf-8")
        self.assertIn("CONFIG_ESP_MAIN_TASK_STACK_SIZE=12288", defaults)

    def test_activation_reboots_restore_board_confirm_state(self) -> None:
        right = (
            ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / "src" /
            "ota_receiver.cpp"
        ).read_text(encoding="utf-8")
        begin = right[
            right.index("void OtaReceiver::begin") :
            right.index("bool OtaReceiver::enterMaintenance")
        ]
        self.assertIn("esp_ota_get_state_partition", begin)
        self.assertIn("ESP_OTA_IMG_PENDING_VERIFY", begin)
        self.assertIn("State::PendingConfirm", begin)

        left = (
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "components" /
            "makcm_core" / "ota_update_manager.cpp"
        ).read_text(encoding="utf-8")
        left_begin = left[
            left.index("bool OtaUpdateManager::begin") :
            left.index("bool OtaUpdateManager::takeRestartRequest")
        ]
        self.assertIn("esp_ota_get_state_partition", left_begin)
        self.assertIn("ESP_OTA_IMG_PENDING_VERIFY", left_begin)
        self.assertIn("makcm::ota::State::PendingConfirm", left_begin)

    def test_right_activate_ack_reopens_only_the_right_bootstrap_gate(self) -> None:
        source = (
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "main" /
            "app_main.cpp"
        ).read_text(encoding="utf-8")
        bridge_start = source.rindex("void bridgeOtaFrame")
        bridge_response = source[
            bridge_start : source.index("void secondaryOtaFrame", bridge_start)
        ]
        # ACTIVATE ACK means the right board is about to reboot and lose its
        # maintenance latch.  Reset only UART1's binary gate so the next host
        # STATUS is forced back through OTA_MAINT_BEGIN; CH343/global quiesce
        # remains held until a terminal fact arrives.
        self.assertIn("const bool exact_activate_ack", bridge_response)
        activate = bridge_response[
            bridge_response.index(
                "if (exact_exit_ack || exact_terminal_ack || exact_activate_ack)"
            ) :
            bridge_response.index("secondary.sendOtaFrame(frame)")
        ]
        self.assertIn("bridge.setOtaMaintenance(false)", activate)
        self.assertIn("right_maintenance_transaction = 0", activate)
        self.assertNotIn("secondary.setOtaMaintenance(false)", activate)
        self.assertNotIn("ota_quiesced.store(false", activate)
        self.assertNotIn("ota_release_authorized.store(true", activate)

        secondary_start = source.index("void secondaryOtaFrame")
        right_route = source[
            source.index("frame.target == makcm::ota::Target::Right", secondary_start) :
            source.index("sendOtaNack(frame, makcm::ota::NackReason::WrongTarget)", secondary_start)
        ]
        self.assertIn("frame.command == makcm::ota::Command::Status", right_route)
        self.assertIn("startRightMaintenanceBootstrap(frame)", right_route)

    def test_exact_terminal_right_ack_authorizes_release_after_lost_response_retry(self) -> None:
        source = (
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "main" /
            "app_main.cpp"
        ).read_text(encoding="utf-8")
        forward = source[
            source.index("bool forwardRightOtaRequest") :
            source.index("bool startRightMaintenanceBootstrap")
        ]
        bridge_start = source.rindex("void bridgeOtaFrame")
        bridge_response = source[
            bridge_start : source.index("void secondaryOtaFrame", bridge_start)
        ]
        right = (
            ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / "src" /
            "ota_receiver.cpp"
        ).read_text(encoding="utf-8")
        right_handle = right[
            right.index("void OtaReceiver::handleFrame") :
            right.index("void OtaReceiver::feed")
        ]

        # A retransmitted ABORT/CONFIRM replaces the expected tuple with the
        # same command/tx/sequence, so the right receiver's cached ACK still
        # grants the terminal transition after the original response was lost.
        self.assertIn("right_expected_command = request.command", forward)
        self.assertLess(
            right_handle.index("requestMatchesCache(frame)"),
            right_handle.index("switch (frame.command)"),
        )
        self.assertIn("writeWire(responseCacheWire_, responseCacheLength_)", right_handle)
        for evidence in (
            "frame.flags == makcm::ota::kFlagResponse",
            "frame.transaction_id == right_expected_transaction",
            "frame.sequence == right_expected_sequence",
            "decodeCommandResponsePayload",
            "terminal_status.last_command == expected_command",
            "terminal_status.state == makcm::ota::State::Idle",
            "terminal_status.reason == makcm::ota::NackReason::None",
        ):
            self.assertIn(evidence, bridge_response)
        self.assertIn("expected_command == makcm::ota::Command::Abort", bridge_response)
        self.assertIn("expected_command == makcm::ota::Command::Confirm", bridge_response)
        self.assertIn("expected_command == makcm::ota::Command::ExitMaintenance",
                      bridge_response)
        self.assertIn("frame.offset == terminal_status.next_offset",
                      bridge_response)
        terminal = bridge_response[
            bridge_response.index("if (exact_exit_ack || exact_terminal_ack)") :
            bridge_response.index("secondary.sendOtaFrame(frame)")
        ]
        self.assertIn("ota_release_authorized.store(true", terminal)
        self.assertIn("right_link_reset_at_ms.store", terminal)
        self.assertIn("bridge.setOtaMaintenance(false)", terminal)
        self.assertIn("right_maintenance_transaction = 0", terminal)
        image_cases = forward[forward.index("switch (request.command)"):
                              forward.index("right_response_expected = true")]
        self.assertNotIn("Command::ExitMaintenance", image_cases)

    def test_left_rejects_exit_without_clearing_a_transfer(self) -> None:
        manager = (
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "components" /
            "makcm_core" / "ota_update_manager.cpp"
        ).read_text(encoding="utf-8")
        handle = manager[manager.index("bool OtaUpdateManager::handle"):
                         manager.index("bool OtaUpdateManager::enter")]
        exit_branch = handle[
            handle.index("request.command == Command::ExitMaintenance"):
            handle.index("request.command == Command::Abort")
        ]
        self.assertIn("reason = NackReason::InvalidState", exit_branch)
        self.assertNotIn("clearTransfer", exit_branch)
        generic_clear = handle[handle.index("if (!ok &&"):]
        self.assertIn("isTransferMutation(request.command)", generic_clear)

    def test_idle_status_probe_is_observational_on_right(self) -> None:
        right = (
            ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / "src" /
            "ota_receiver.cpp"
        ).read_text(encoding="utf-8")
        handle_start = right.index("void OtaReceiver::handleFrame")
        status_case = right[
            right.index("case Command::Status:", handle_start) :
            right.index("case Command::Activate:", handle_start)
        ]
        self.assertIn("fillStatusV2", status_case)
        self.assertNotIn("releaseMaintenance", status_case)
        self.assertNotIn("maintenanceActive_", status_case)

    def test_crash_rerun_status_bypasses_only_transaction_binding(self) -> None:
        left = (
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "main" /
            "app_main.cpp"
        ).read_text(encoding="utf-8")
        secondary_start = left.index("void secondaryOtaFrame")
        right_route = left[
            left.index("frame.target == makcm::ota::Target::Right", secondary_start) :
            left.index("sendOtaNack(frame, makcm::ota::NackReason::WrongTarget)", secondary_start)
        ]
        direct_gate = right_route[
            right_route.index("if (bridge.otaMaintenance()") :
            right_route.index("else if", right_route.index("if (bridge.otaMaintenance()"))
        ]
        self.assertIn("frame.command == makcm::ota::Command::Status", direct_gate)
        self.assertIn("frame.command == makcm::ota::Command::Abort", direct_gate)
        self.assertIn("frame.transaction_id == right_maintenance_transaction", direct_gate)
        self.assertIn("forwardRightOtaRequest(frame)", right_route)
        self.assertNotIn("right_maintenance_transaction = frame.transaction_id", right_route)

        bootstrap_gate = right_route[
            right_route.index("else if", right_route.index("if (bridge.otaMaintenance()")):
        ]
        self.assertIn("frame.command == makcm::ota::Command::ExitMaintenance",
                      bootstrap_gate)
        self.assertNotIn("frame.command == makcm::ota::Command::Abort",
                         bootstrap_gate)
        self.assertNotIn("frame.command == makcm::ota::Command::Confirm",
                         bootstrap_gate)

        right = (
            ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / "src" /
            "ota_receiver.cpp"
        ).read_text(encoding="utf-8")
        handle_start = right.index("void OtaReceiver::handleFrame")
        status_case = right[
            right.index("case Command::Status:", handle_start) :
            right.index("case Command::Activate:", handle_start)
        ]
        self.assertIn("!isMaintenanceActive()", status_case)
        self.assertNotIn("sessionTransactionId_", status_case)

        # Every mutating case remains bound to the recovered old transaction.
        mutations = right[
            right.index("case Command::Manifest:", handle_start) :
            right.index("default:", handle_start)
        ]
        self.assertGreaterEqual(mutations.count("sessionTransactionId_"), 3)
        self.assertIn("frame.transaction_id != transactionId_", mutations)

    def test_empty_maintenance_abort_releases_without_image_state(self) -> None:
        right = (
            ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / "src" /
            "ota_receiver.cpp"
        ).read_text(encoding="utf-8")
        frame_start = right.index("void OtaReceiver::handleFrame")
        abort_case = right[
            right.index("case Command::Abort:", frame_start) :
            right.index("case Command::Confirm:", frame_start)
        ]
        self.assertIn("accepted = abortUpdate(Ok)", abort_case)
        self.assertIn("releaseMaintenanceAfterResponse = accepted", abort_case)
        self.assertNotIn("snapshot_", abort_case)

    def test_left_release_requires_terminal_board_authorization(self) -> None:
        left = (
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "main" /
            "app_main.cpp"
        ).read_text(encoding="utf-8")
        start = left.index("bool tryReleaseOtaQuiesce()")
        release = left[start:left.index("bool otaPeerHealth", start)]
        self.assertIn("ota_quiesced.load", release)
        self.assertIn("ota_manager.maintenanceExclusive()", release)
        self.assertIn("right_response_expected", release)
        self.assertIn("ota_release_authorized.load", release)
        self.assertIn("ota_manager.state() == makcm::ota::State::Idle", release)
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
        self.assertNotIn("bootstrap_only_session", release)

    def test_right_status_never_authorizes_global_release(self) -> None:
        left = (
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "main" /
            "app_main.cpp"
        ).read_text(encoding="utf-8")
        bridge_start = left.rindex("void bridgeOtaFrame")
        bridge = left[bridge_start:left.index("void secondaryOtaFrame",
                                               bridge_start)]
        status_cache = bridge[
            bridge.index("const bool valid_status_v2"):
            bridge.index("makcm::ota::CommandResponsePayload")
        ]
        self.assertNotIn("idle_maintenance_status", status_cache)
        self.assertNotIn("ota_release_authorized.store(true", status_cache)

    def test_right_response_deadline_drops_only_peer_gate(self) -> None:
        left = (
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "main" /
            "app_main.cpp"
        ).read_text(encoding="utf-8")
        forward = left[left.index("bool forwardRightOtaRequest"):
                       left.index("bool startRightMaintenanceBootstrap")]
        self.assertIn("right_response_started_ms = nowMs()", forward)
        self.assertIn("kRightResponseTimeoutMs", left)

        timeout_start = left.index("if (right_response_expected &&")
        timeout = left[timeout_start:left.index("bool restart_left", timeout_start)]
        for evidence in (
            "bridge.setOtaMaintenance(false)",
            "right_response_expected = false",
            "right_response_started_ms = 0",
            "right_expected_transaction = 0",
            "right_expected_sequence = 0",
            "right_expected_command = makcm::ota::Command::Hello",
            "right_maintenance_transaction = 0",
            "ota_release_authorized.store(false",
        ):
            self.assertIn(evidence, timeout)
        self.assertNotIn("secondary.setOtaMaintenance(false)", timeout)
        self.assertNotIn("ota_quiesced.store(false", timeout)
        self.assertNotIn("ota_manager.", timeout)

    def test_release_retry_uses_guarded_release_helper(self) -> None:
        left = (
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "main" /
            "app_main.cpp"
        ).read_text(encoding="utf-8")
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
        release = left[start:left.index("bool otaPeerHealth", start)]
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
            right.index("bool OtaReceiver::fillStatusV2") :
            right.index("void OtaReceiver::cacheRequest")
        ]
        self.assertIn("response.offset = status.next_offset;", status)
        self.assertIn("status.next_offset = bytesWritten_;", status)
        self.assertIn("response.sequence = request.sequence;", status)

    def test_left_relays_correlated_right_response_verbatim(self) -> None:
        """The bridge correlates by tx+seq, then relays; it never vetoes.

        The old path validated the decoded payload and replaced a mismatch
        with a compact command-response NACK without Status V2, which the
        detailed-status host filter drops, so every right-board response read
        as a timeout. In host-driven OTA the host owns response interpretation;
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
        # No veto and no substitute NACK on this path.
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
            right.index("void OtaReceiver::cacheRequest")
        ]
        self.assertIn("status.last_command = Command::Status;", right_fill)
        self.assertIn(
            "status.last_response_command = Command::StatusResponse;", right_fill
        )
        for fill in (left_fill, right_fill):
            self.assertIn("response.command = Command::StatusResponse", fill)
            self.assertIn("kFlagResponse | kFlagStatusV2", fill)
            self.assertIn("response.length = kStatusV2PayloadSize", fill)
        self.assertNotIn("ledger", right_fill)

    def test_compact_response_has_no_retired_status_payload_names(self) -> None:
        production = [
            ROOT / "common" / "OtaProtocol.h",
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "components" /
            "makcm_core" / "ota_update_manager.cpp",
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "main" /
            "app_main.cpp",
            ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / "src" /
            "ota_receiver.cpp",
            ROOT / "tools" / "makcm_ota_tool.py",
        ]
        combined = "\n".join(path.read_text(encoding="utf-8")
                             for path in production)
        # Build retired identifiers from fragments so repository-wide searches
        # can prove the old production spellings are completely absent.
        for retired in ("Status" + "Payload", "kStatus" + "PayloadSize",
                        "encodeStatus" + "Payload",
                        "decodeStatus" + "Payload",
                        "decode_status" + "_payload"):
            self.assertNotIn(retired, combined.replace("StatusPayloadV2", ""))
        self.assertIn("CommandResponsePayload", combined)
        self.assertIn("decode_command_response_payload", combined)

    def test_shared_ota_state_is_the_native_four_state_contract(self) -> None:
        protocol = (ROOT / "common" / "OtaProtocol.h").read_text(
            encoding="utf-8"
        )
        state = protocol[
            protocol.index("enum class State") :
            protocol.index("};", protocol.index("enum class State"))
        ]
        for current in ("Idle", "Receiving", "Prepared", "PendingConfirm"):
            self.assertIn(current, state)
        for retired in (
            "Maintenance", "Fault", "Failed", "Activated", "Confirmed",
            "ActivationPending", "ActivateIntent", "AwaitConfirm",
        ):
            self.assertNotIn(retired, state)

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
        self.assertIn("PeerHealthCheck", header)
        local_lambda = tasks[tasks.index("otaReceiver.begin(") :]
        self.assertLess(
            local_lambda.index("cleanupComplete != nullptr"),
            local_lambda.index("hostInstallStatus == ESP_OK"),
        )
        confirm = receiver[
            receiver.index("bool OtaReceiver::handleConfirm") :
            receiver.index("void OtaReceiver::handleFrame")
        ]
        self.assertIn("localHealthPassed()", confirm)
        self.assertIn("healthPassed()", confirm)

    def test_right_rollback_has_no_previous_slot_guessing(self) -> None:
        receiver = (
            ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / "src" /
            "ota_receiver.cpp"
        ).read_text(encoding="utf-8")
        self.assertNotIn("previous_slot", receiver)
        self.assertNotIn("esp_ota_get_next_update_partition", receiver[
            receiver.index("case Command::Abort:") :
            receiver.index("case Command::Confirm:")
        ])
        self.assertIn("esp_ota_mark_app_invalid_rollback_and_reboot", receiver)

    def test_right_abort_failure_cannot_release_maintenance(self) -> None:
        receiver = (
            ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / "src" /
            "ota_receiver.cpp"
        ).read_text(encoding="utf-8")
        handle = receiver[
            receiver.index("void OtaReceiver::handleFrame") :
            receiver.index("void OtaReceiver::feed")
        ]
        self.assertIn("accepted = abortUpdate(Ok)", handle)
        self.assertIn("if (!accepted) error = Flash", handle)
        self.assertIn("releaseMaintenanceAfterResponse = accepted", handle)

    def test_right_ota_initialization_failure_refuses_maintenance(self) -> None:
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
        self.assertIn("initialized_ =", begin)
        self.assertIn("if (!initialized_)", begin)
        enter = receiver[
            receiver.index("bool OtaReceiver::enterMaintenance") :
            receiver.index("bool OtaReceiver::peerCompatible")
        ]
        self.assertIn("MaintenanceReject::NotInitialized", enter)
        self.assertNotIn("maintenanceActive_.store(true", begin)

    def test_data_replay_binds_sequence_and_sha256_on_both_boards(self) -> None:
        right = (
            ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / "src" /
            "ota_receiver.cpp"
        ).read_text(encoding="utf-8")
        left = (
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "components" /
            "makcm_core" / "ota_update_manager.cpp"
        ).read_text(encoding="utf-8")
        self.assertIn("requestMatchesCache", right)
        self.assertIn("requestCache_.sequence", right)
        self.assertIn("requestCache_.payloadSha256", right)
        self.assertIn("kFlagRetransmit", right)
        self.assertIn("sha256", right.lower())

    def test_response_cache_requires_explicit_retransmit_on_both_boards(self) -> None:
        right = (
            ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / "src" /
            "ota_receiver.cpp"
        ).read_text(encoding="utf-8")
        left = (
            ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "components" /
            "makcm_core" / "ota_update_manager.cpp"
        ).read_text(encoding="utf-8")

        for source, signature in (
            (right, "bool OtaReceiver::requestMatchesCache"),
            (left, "bool OtaUpdateManager::requestMatchesCache"),
        ):
            body = source[source.index(signature):]
            body = body[:body.index("\n}\n")]
            self.assertIn(
                "(request.flags & makcm::ota::kFlagRetransmit) == 0",
                body,
            )
            self.assertIn(
                "request.flags & ~makcm::ota::kFlagRetransmit",
                body,
            )

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


    def test_reboot_drops_transient_transaction_ownership(self) -> None:
        source = (
            ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / "src" / "ota_receiver.cpp"
        ).read_text(encoding="utf-8")
        begin = source[
            source.index("void OtaReceiver::begin"):
            source.index("bool OtaReceiver::enterMaintenance")
        ]
        self.assertIn("state_ = State::Idle", begin)
        self.assertIn("ESP_OTA_IMG_PENDING_VERIFY", begin)
        self.assertNotIn("transactionId_ =", begin)
        body = source[source.index("bool OtaReceiver::enterMaintenance("):]
        body = body[: body.index("\n}\n")]
        self.assertIn("if (isMaintenanceActive())", body)
        self.assertIn("sessionTransactionId_ = transactionId", body)
        self.assertNotIn("transactionId != sessionTransactionId_", body)
        for site in ("NotInitialized", "ZeroTransaction", "SerialBusy"):
            self.assertIn(f"MaintenanceReject::{site}", body)
        self.assertNotIn("ActiveTransactionMismatch", body)


if __name__ == "__main__":
    unittest.main()
