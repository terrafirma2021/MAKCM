# MAKCU 2024
**Open Source ESP32-S3 MAKCU (Kmbox Alternative)**

![MAKCU 2024 Image](https://github.com/user-attachments/assets/02ddc456-4abc-4676-8121-7f3084370923)

Welcome to the MAKCU 2024 project! This is a new open-source implementation for remote injection of mouse control with passthrough, featuring dual ESP32-S3 microcontrollers based on RTOS and advanced task management over the traditional loop.

The firmware performs the one-time internal-PHY eFuse setup automatically on
first boot. Normal signed pair OTA does not require holding BOOT or reconnecting
either board in ROM download mode.


## Key Features

- **High-Speed Communication**: Uses a framed 5 Mbps MCU-to-MCU link.
- **Dual ESP32-S3 MCUs**: Utilizing two microcontrollers for enhanced processing power and efficiency.
- **RTOS-Based Task Management**: Using real-time operating system (RTOS) tasks over the conventional loop for better multitasking and system management.
- **Automatic HID Report Descriptor Parsing**: No need for custom firmware; the system automatically parses HID report descriptors.
- **Descriptor-Driven Proxy**: Supports compatible mice, keyboards, HID
  controllers, DSE and raw vendor/XInput-style interfaces without selecting a
  firmware personality by VID/PID.
- **Internal PHY in OTG Mode**: The firmware performs the required one-time
  eFuse setup on first boot.
- **Ample Storage**: 
  - 4MB Quad SPI Flash per MCU
  - 2MB PSRAM per MCU
- **JSON Streaming for USB Descriptors**: All USB descriptors are easily accessible and followable via JSON streaming.
- **Clean Codebase**: Built with a focus on maintainability and readability.
- ** Espressif API ** The largest community based MCU team in the world!

## Roadmap

- Expand controller support beyond the documented Full-Speed topology matrix.
- Expand HID compatibility and features.
- Continue to optimize the RTOS tasks for more efficient use.
- Implement additional security measures.

## Descriptor-driven USB proxy

The two MCUs now proxy the common USB transport used by mice, keyboards and
game controllers instead of selecting a firmware path from VID/PID:

```text
USB device -> right MCU host -> 5 Mbps framed UART -> left MCU device -> PC/XIM
PC/XIM endpoint/control output <- same endpoint/control transaction tunnel -> device
```

- The right MCU copies the complete 18-byte Device Descriptor, active
  Configuration Descriptor and every HID Report Descriptor. It claims every
  alternate-setting-zero interface and opens all supported IN/OUT endpoints.
- The left MCU returns those raw descriptors unchanged. A TinyUSB application
  class driver is placed ahead of the built-in HID/Vendor drivers and claims
  the original interfaces and endpoint addresses without knowing their class.
- Every UART data frame contains the original endpoint address followed by the
  unmodified transfer. Class and vendor EP0 requests use a transaction ID and
  are forwarded in both directions; this covers HID Feature/Output reports,
  XInput output, vibration and device-specific control requests.
- The link uses protocol-v5 COBS frames with a physical-connection session ID,
  sequence number and CRC16-CCITT. Stale reports from a removed device cannot
  enter the next device session.
- Rapid downstream re-enumeration is queued while the old host handle is
  cleaned up. The right MCU no longer restarts merely because a controller
  changes USB persona during connection.
- The current hardware/firmware supports Control plus Full-Speed Interrupt and
  Bulk endpoints with packet sizes up to 64 bytes and endpoint numbers 1-5.
  Isochronous audio/headset interfaces and alternate-setting streaming are
  intentionally rejected instead of being partially exposed.
- DSE triggers are also exposed on the CH343 control port using the button
  stream consumed by SecondaryAim: R2 maps to bit 0 (fire/LMB), L2 maps to bit
  1 (ADS/RMB). Press/release hysteresis is 24/12 and a 100 ms DSE-report
  timeout fails closed. `km.buttons(1,period)`, `km.version()`, and the 4 Mbps
  speed-switch blob are supported. This is a side-band observer and does not
  modify the DSE report or descriptor.
- Physical Compx/Boot Mouse button bits are derived directly from the framed
  raw HID input report on the left MCU, so SecondaryAim observes the same
  `km.<mask>` semantics for mouse L/R as for DSE R2/L2 without relying on an
  unprotected text side channel.
- `km.move` injects on an existing parseable signed-relative HID mouse
  interface (Boot Mouse is the fallback). It does not
  append a hidden mouse collection to a controller descriptor; if the physical
  topology has no mouse interface, descriptor fidelity takes precedence and no
  synthetic mouse endpoint is invented.
- ESP32-S3 USB Host is Full-Speed. An 8 kHz-capable receiver therefore operates
  at the Full-Speed frame rate rather than retaining a High-Speed 8 kHz poll.

### Build verification

Both firmware projects use the same ESP-IDF 5.5.4 toolchain. The right project
locks Arduino-ESP32 3.3.10 as an ESP-IDF managed component.

```bash
idf.py -C MAKCM_ESP32s3_HID_Mouse_Right build
idf.py -C MAKCM_ESP32s3_Device_Mouse_Left_IDF build
for src in tests/test_bridge_protocol.cpp tests/test_endpoint_tx_phase.cpp \
           tests/test_hid_mouse_report.cpp \
           tests/test_mouse_injection_ledger.cpp; do
  out=/tmp/$(basename "$src" .cpp)
  g++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
      -Icommon "$src" -o "$out" && "$out"
done
```

The scheduler, Windows/XInput boundary, supported USB matrix, identity limits
and stress-test gates are documented in
[`docs/PROXY_ARCHITECTURE_AND_ACCEPTANCE.md`](docs/PROXY_ARCHITECTURE_AND_ACCEPTANCE.md).

## Updates

### Left MCU:
- Implemented 4-stage debug logging.
- Both MCUs now share a COM port for debug logging.
- Made bottom MCU COM port redundant on DIY dev kit.


### General Changes:
- Migrated to ISR Trigger for both UART0, UART1 (Hybrid Arduino/RTOS).
- Removed `vTaskDelay(1)` on UART0 and UART1, enabling full-speed logging using interrupt triggers.
- Verified and reduced task stack sizes (All tasks have a 500-byte high-water mark breathing space).
- Replaced latest-value `km.move` overwrites with a checked relative-integer
  delivery ledger; USB completion, not UART parsing, retires movement.
- Implemented button state logging:
  - Ensures that if a button is already up/down, any other sends will be ignored.
  - Results in better mouse feel and avoids random clicks.
  - added commands to adjust com port speed at any time for cheats that support (115k-5mpbs) 

### Code Improvements:
- Migrated tasks to separate `.cpp` file for a cleaner look.
- Tidied up `main.cpp`.
- General formatting improvements.
- Removed redundant code.

### Right MCU:

- Migrated ESPLOG to seperate log function to pass over uart1
- Set LED to use a task of its own (reducing usb task by 1024 bytes)
- Ongone: Fixed usb_host_transfer_free bug, sometimes API reponds invalid state (USB wont reconnect)
- All tasks verified with high-water mark (all tasks have 500 breathing space)
- general tidy up 

## Simple Debug log tool in python
- Run the tool follow the simple instuctions and get full debug log for any issues handling debive



## Community and Support

Join our [Discord community](https://discord.gg/6TJBVtdZbq) to engage with other developers, share your experiences, and get support from the community.

We are excited to have you on board and look forward to your contributions!

---

Feel free to contribute, raise issues, or suggest enhancements. Together, we can make MAKCU 2024 a robust and versatile tool for everyone.

## Contributing

Please refer to the [Contributing Guidelines](CONTRIBUTING.md) to get started. Contributions are always welcome!

## License

This project is licensed under the [MIT License](LICENSE).
