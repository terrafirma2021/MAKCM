from __future__ import annotations

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
IDF_ROOT = ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF"


def _read_kconfig(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        match = re.fullmatch(r"(CONFIG_[A-Z0-9_]+)=(.*)", line)
        if match:
            values[match.group(1)] = match.group(2)
            continue
        match = re.fullmatch(r"# (CONFIG_[A-Z0-9_]+) is not set", line)
        if match:
            values[match.group(1)] = "n"
    return values


class LeftIdfControlUartConfigTest(unittest.TestCase):
    def _assert_protocol_uart_is_console_free(self, config: dict[str, str]) -> None:
        self.assertEqual(config.get("CONFIG_ESP_CONSOLE_NONE"), "y")
        self.assertEqual(config.get("CONFIG_ESP_CONSOLE_UART_DEFAULT"), "n")
        self.assertEqual(config.get("CONFIG_ESP_CONSOLE_UART_CUSTOM", "n"), "n")
        self.assertEqual(config.get("CONFIG_ESP_CONSOLE_SECONDARY_NONE"), "y")
        # The constraint is "nothing but the protocol on UART0", not "no
        # postmortem evidence anywhere".  Coredump to UART would corrupt command
        # framing; coredump to flash uses a partition the A/B schema already
        # reserves and is the only postmortem channel a board with no console
        # has.  Without it a panic is indistinguishable from a silent peer.
        self.assertEqual(config.get("CONFIG_ESP_COREDUMP_ENABLE_TO_UART"), "n")
        self.assertEqual(config.get("CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH"), "y")
        self.assertEqual(config.get("CONFIG_ESP_COREDUMP_ENABLE_TO_NONE", "n"), "n")

    def test_persistent_defaults_reserve_uart0_for_control_protocol(self) -> None:
        self._assert_protocol_uart_is_console_free(
            _read_kconfig(IDF_ROOT / "sdkconfig.defaults")
        )

    def test_generated_build_config_matches_persistent_defaults(self) -> None:
        generated = IDF_ROOT / "sdkconfig"
        if not generated.exists():
            self.skipTest("generated sdkconfig is absent before the first IDF configure")
        self._assert_protocol_uart_is_console_free(_read_kconfig(generated))

    def test_control_protocol_still_uses_explicit_ch343_pins(self) -> None:
        source = (IDF_ROOT / "main" / "app_main.cpp").read_text(encoding="utf-8")
        self.assertIn("secondary_config.port = UART_NUM_0;", source)
        self.assertIn("secondary_config.tx_pin = 43;", source)
        self.assertIn("secondary_config.rx_pin = 44;", source)
        self.assertIn("secondary_config.initial_baud = 115200;", source)
        self.assertIn("secondary_config.fast_baud = 4000000;", source)
        self.assertIn("secondary_config.task_stack = 12288;", source)
        self.assertIn("secondary.minimumFreeTaskStackBytes()", source)

    def test_ota_maintenance_does_not_block_baud_recovery(self) -> None:
        source = (
            IDF_ROOT / "components" / "makcm_core" / "secondary.cpp"
        ).read_text(encoding="utf-8")
        handle = source[source.index("void Secondary::handleByte") :]
        handle = handle[: handle.index("void Secondary::handleLine")]
        # A board that reboots into pending-confirm/fault maintenance comes up
        # at 115200 and must still be reachable by the 4 M Linux updater, so the
        # switch sequence cannot sit behind any maintenance gate.  There is no
        # longer a text gate at all -- maintenance owns the data plane, not the
        # diagnostic plane -- which satisfies this by construction.
        self.assertIn("if (byte == kSpeedSwitch[switch_index_])", handle)
        self.assertNotIn(
            "if (ota_maintenance_.load(std::memory_order_acquire)) return;",
            handle,
        )

    def test_idle_board_accepts_mkot_magic_before_exclusive_maintenance(
        self,
    ) -> None:
        source = (
            IDF_ROOT / "components" / "makcm_core" / "secondary.cpp"
        ).read_text(encoding="utf-8")
        handle = source[source.index("void Secondary::handleByte") :]
        magic_arm = handle.index("byte == makcm::ota::kMagic[0]")
        # Decoder arming must not require ota_maintenance_; that gate made Idle
        # HELLO time out while text km.* still answered.
        gated = (
            "if (ota_maintenance_.load(std::memory_order_acquire) &&\n"
            "        (ota_decoder_.buffered() != 0 ||\n"
            "         (line_length_ == 0 && byte == makcm::ota::kMagic[0]))"
        )
        self.assertNotIn(gated, handle)
        self.assertLess(
            handle.index("ota_decoder_.buffered() != 0"),
            handle.index("if (byte == kSpeedSwitch[switch_index_])"),
        )
        self.assertLess(
            magic_arm,
            handle.index("if (byte == kSpeedSwitch[switch_index_])"),
        )

    def test_ota_maintenance_drains_control_uart_without_per_chunk_delay(
        self,
    ) -> None:
        source = (
            IDF_ROOT / "components" / "makcm_core" / "secondary.cpp"
        ).read_text(encoding="utf-8")
        task = source[source.index("void Secondary::task()") :]
        task = task[: task.index("bool Secondary::setOtaMaintenance")]

        # Keep the idle read blocking for a real 100 Hz FreeRTOS tick, while
        # preserving the established byte handler and stack telemetry path.
        self.assertRegex(
            task,
            r"uart_read_bytes\(config_\.port, buffer, sizeof\(buffer\),\s*"
            r"pdMS_TO_TICKS\(10\)\)",
        )
        handler = "for (int i = 0; i < count; ++i) handleByte(buffer[i]);"
        stack_update = "updateMinimumFreeTaskStackBytes();"
        maintenance_gate = (
            "if (!ota_maintenance_.load(std::memory_order_acquire))\n"
            "                vTaskDelay(1);"
        )
        self.assertIn(handler, task)
        self.assertIn(maintenance_gate, task)
        self.assertLess(
            task.index(handler),
            task.index(stack_update, task.index(handler)),
        )
        self.assertLess(
            task.index(stack_update, task.index(handler)),
            task.index(maintenance_gate),
        )
        self.assertEqual(task.count("vTaskDelay(1);"), 1)


if __name__ == "__main__":
    unittest.main()
