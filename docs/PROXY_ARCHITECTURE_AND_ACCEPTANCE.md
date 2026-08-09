# USB proxy architecture and acceptance criteria

## Implemented in the current IDF/Arduino pair

The current branch now has the first generic-proxy boundary in code:

- `ProfileStore` validates the raw device/configuration topology before D+ is
  enabled and reports a `ProxyCapability` result.
- The right host optionally prefetches the Microsoft OS 1.0 Extended Compat ID
  response using the vendor code from string index `0xEE`; it is transferred as
  a profile control-cache chunk.
- The left RAW driver serves a matching cached control-IN request locally, so
  enumeration does not wait on a UART/host round trip.
- A bridge input frame cannot implicitly configure an endpoint.  Only the RAW
  driver's successful `open()` path owns endpoint state, and `ProfileAccepted`
  is held until every profiled non-control endpoint is open.
- Every non-mouse IN endpoint uses its own ordered FIFO.  There is no
  VID/PID-specific latest-wins or endpoint-priority path in the transport;
  endpoint/class interpretation is optional policy only.
- The optional known-device trigger/mouse interpretation is isolated in
  `InputPolicy`; an unrecognized profile never enters that code path.
- OUT control data uses a self-contained `ControlOutData` frame, and mount/
  unmount lifecycle is reported with `SetConfiguration`.
- `ProfileAccepted` and `StreamStarted` are separate gates: the right host
  opens its input queues only after the left confirms `StreamReady`, and it
  retries a lost `StreamStarted` frame.
- Deferred EP0 state is protected by a short cross-core critical section;
  response copying and request snapshots are separated from UART/TinyUSB
  calls, so a timer or bridge response cannot observe a half-written request.

These changes are compiled and covered by the host `ProfileStore`, capability,
policy and complete 4-IN/2-OUT profile tests, but the physical acceptance gates
below still require a signed pair release and USBPcap/Windows measurements.

Application deployment uses only `tools/update_proxy_pair.py` through the left
board CH343. It completes and health-checks Right before beginning Left. Build,
signing and deployment details are in `docs/OTA_HANDOFF_20260806.md`.

## Trigger condition and remaining blocker

Secondary treats the observed trigger/button mask as an arm gate and can emit
`km.move(dx,dy)` at 1000 Hz while it is held. The old left-side scheduler put
synthetic mouse reports and every physical USB IN endpoint into one queue. That
head-of-line problem has been removed: every endpoint now has an independent
FIFO and fair drain opportunity. The controller can nevertheless still stall
during high-rate mouse injection on Windows, so endpoint contention versus
host/application input pressure remains a hardware acceptance item. Do not
infer the root cause from the old scheduler alone.

## Current data paths

```text
physical device
  -> right ESP32-S3 USB host
  -> protocol-v5 COBS/CRC UART frame at 5 Mbps
  -> left per-endpoint FIFO and transfer state
  -> original endpoint/address/report on PC, Windows, Linux or XIM

Secondary km.move
  -> checked int64 relative ledger
  -> merge into the next physical mouse report when it fits
  -> otherwise drain a descriptor-sized relative report on a mouse poll
  -> retire the contribution only after a full successful USB completion
```

Each IN endpoint has an independent `Idle -> Starting -> InFlight ->
Completing` state and session epoch. A busy mouse endpoint does not wait in
front of XInput, keyboard or vendor reports. A submit failure, failed
completion or completion shorter than the submitted report does not retire
mouse movement. Physical input is retried. A 7-byte report on a 64-byte endpoint
is valid when the submitted and completed lengths are both 7.

The relative ledger never converts to absolute coordinates. Its invariant is:

```text
accepted relative integer sum
  = successfully completed sum + current pending sum
```

Buttons are a latch. Every mouse report exposes `physical_buttons |
injected_buttons`; repeated down/up commands are idempotent. A USB bus reset
keeps a held latch but marks it dirty for the newly enumerated host. A physical
device/session removal clears the ledger and latch.

## Secondary contract

The firmware retains the exact contract used by
`aim/src/output/MakcuDevice.cpp`:

```text
115200: DE AD 05 00 A5 00 09 3D 00
reopen: 4,000,000 baud
km.version()
km.echo(0)
km.buttons(1,8)
km.buttons(1)
km.move(dx,dy)
km.left/right/middle/side1/side2(0|1)
```

The button wire frame remains `"km." + one binary mask byte + "\r\n>>> "`.
Bit 0 is physical mouse LMB or controller R2; bit 1 is RMB or L2. DSE/Xbox
trigger hysteresis is press >= 24, release <= 12, with a 100 ms controller
report timeout.

Secondary requests an 8 ms periodic button stream and now fails closed when it
has not received a mask for 80 ms. This prevents a lost release or stalled
stream from continuing the 1000 Hz movement producer indefinitely.

## Windows and XInput

The proxy does not convert a controller into XInput. It reproduces the original
Device, active Configuration, HID Report and cached String descriptors, opens
the original endpoint addresses, and forwards raw vendor-class XInput traffic.
Windows can bind the same driver only when the physical device's original
VID/PID/topology is already supported and lies inside the matrix below.

XInput raw IN is independent of mouse injection. Endpoint OUT reports (rumble,
LED and similar output) are queued outside TinyUSB's completion callback and
forwarded to the right MCU. Control OUT data uses the self-contained
`ControlOutData` frame; dynamic control IN/OUT responses carry request IDs,
short-packet/ZLP flags, STALL status and a bounded EP0 timeout.

## Supported USB matrix

- Full-Speed control plus interrupt/bulk endpoints
- `bcdUSB` up to 2.00 (no BOS-capable USB 2.1+ identity)
- one configuration with any non-zero `bConfigurationValue`, alternate setting zero
- endpoint numbers 1 through 6
- maximum packet size 64 bytes
- HID mice with a parseable signed relative X/Y report, with or without a
  Report ID; Boot Mouse is the fallback
- raw HID, DSE and vendor-class/XInput interfaces fitting those constraints

Rejected rather than partially impersonated:

- High-Speed-only behavior (including native 8 kHz polling)
- isochronous audio/headset interfaces
- alternate-setting streams, multiple configurations, packet sizes above 64
- devices whose EP0 max packet size is not 64

## Identity and observability boundary

For supported profiles the raw descriptors are authoritative, no extra HID
collection is appended, and the MAKCM target build has CDC disabled. The normal
runtime device therefore exposes the physical device identity rather than an
additional ESP32 debug interface.

This is descriptor fidelity, not a promise of an undetectable proxy. Full-Speed
timing, two-MCU/UART jitter, the supported-topology limits, uncached languages,
large control reports and bootloader/BOOT mode remain observable. `USB_PHY_SEL`
assigns the internal PHY to USB OTG; it does not permanently disable every JTAG
or ROM-download path. Validate compatibility rather than claiming that a target
cannot detect an interposed device.

## Acceptance gates

Run `LEFT_STATUS` on the left and `DEBUG_STATUS` on the right before and after
each stress test. The right-side status is forwarded through the left as
`RIGHT:ESPLOG_STATUS...` lines.

Keep the post-handshake bridge at 5,000,000 baud (the separate Secondary/CH343
link remains at 4,000,000) and collect two status samples about two seconds
apart without resetting either board. The current native left status exposes
profile/capability rejection, endpoint open/transfer counters, EP0
timeout/STALL counters and mouse-injection counters; the right status exposes
per-endpoint input forwarding, queue overwrite and UART errors.

1. Hold R2, L2 and both for 30 seconds while Secondary emits at 1000 Hz. The
   controller must remain usable; left endpoint drops/retries and right
   `overwrite`/`txerr` must remain zero.
2. Send known movement sums including `(40000,-40000)`. After drain, `pend` must
   be `0/0/0/0`; observed relative USB reports must sum exactly to the accepted
   host commands.
3. Run physical 1000 Hz mouse input plus 1000 Hz `km.move` for 10 minutes. Left
   mouse queue drops and right `overwrite`/`txerr` must remain zero.
4. Press/release each button and `00/01/02/03/1F`; masks should arrive every
   8 ms nominally, and a stopped stream must disarm Secondary within 80 ms.
5. Hot-unplug/replug mouse, DSE and supported Xbox mode repeatedly. Session and
   scheduler epoch must advance; stale counters may record rejected late work,
   but no stale input may appear in the new device session.
6. On Windows, compare direct versus proxy Device/Configuration/Report/String
   bytes, Hardware IDs, selected driver, XInput enumeration, rumble/LED output,
   and USBPcap control success/STALL behavior. Descriptor equality alone is not
   sufficient to assert behavioral equality.

For raw descriptor evidence, capture one snapshot with the controller directly
attached and one through the proxy:

```powershell
python tests/usb_descriptor_snapshot.py --vid 0x4131 --pid 0x3519 --out direct.json
python tests/usb_descriptor_snapshot.py --vid 0x4131 --pid 0x3519 --out proxy.json
python tests/compare_usb_descriptor_snapshots.py direct.json proxy.json
```

The host regression suite covers HID merge and ledger arithmetic; sanitizer runs
remain an optional follow-up when the host toolchain exposes compatible ASan/
UBSan runtimes. Both MCU projects must then build in release mode before
producing paired binaries.
