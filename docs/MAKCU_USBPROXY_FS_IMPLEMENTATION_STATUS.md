# MAKCU USBProxy-FS 实施状态

更新时间：2026-08-06

## OTA 状态更正（MKOT v2 / generation 15，2026-08-06）

下方 generation 3→4 的成功记录只保留为历史证据，**不再代表当前 OTA 已达到生产
放行**。后续 generation 6→14 的左板 pending-image 首启均回滚到 generation 5；已知
`otadata` 事实为新槽 `ABORTED`、旧槽 `VALID`。generation 11 的早期 Flash checkpoint
还会自诱发 cache/IPC panic，generation 12 的 coredump 又不完整，因此不能把某一轮
诊断栈当作原始根因。缺少 generation 14 完整失败产物和当前两板只读 readback 前，
实机门禁仍为 **STOP**，不得继续用刷代数试错。

GO-1 只读审核包（2026-08-06，未刷机）：`diagnostics/ota/GO1_READONLY_AUDIT_PACK.md`；
gen15 静态冻结 `diagnostics/ota/go1_freeze_20260806/`；gen14 缺口
`diagnostics/ota/gen14_evidence_inventory.md`；故障注入覆盖
`diagnostics/ota/GO1_FAULT_INJECTION_COVERAGE.md`。当前裁决：**GO-1 未放行**
（缺当前 USB1 descriptor 抓包、缺 signed left `.mkota`、缺实机只读
bootloader/table/otadata readback；gen14 exact 产物仍缺失）。

当前 generation 15 仅完成源码、自动测试和本地构建，尚未刷入：

- 本板 image lifecycle、pair coordinator、Linux journal 分离为三个持久化事实源；
  pair record 只协调，不再代替两板本地 A/B 事实。右板恢复 `RollbackIntent` 时必须证明
  `running==boot==previous slot`、镜像 SHA 和 `VALID` 后才持久化 `RolledBack`；Abort、
  NVS 或响应落盘失败均保留 maintenance/fail-close。
- 右板 USB1/Serial0 明确不可信，只允许普通透传/转换器数据面；OTA bootstrap 仅接受
  左板 UART1。OTA/NVS 初始化失败会同时保留 maintenance 并预留 USB Host，不得以
  “更新服务不可用、普通桥仍运行”的方式 fail-open。
- 左板补齐 right-local-validated phase guard、V1 `StatusResponse` 兼容，以及“右板已
  Prepare、左板从未接受 MANIFEST”时的单边回滚退休：只有右板精确证明 previous slot、
  左板 Status V2 精确证明无 active/completed evidence，才写入有序 `None` coordinator
  记录；不伪造双板 rollback tombstone，也不非原子删除 A/B key。
- DATA 重放除 frame CRC32 外再绑定 sequence 和 SHA-256；旧 offset 的右板恢复路径仍
  逐字节读回目标分区比较，避免 CRC32 碰撞被当作幂等重放。
- Linux 编排器在任何写入前读取并持久化左右精确 Status V2；confirmed no-op 同时要求
  两板 completed tombstone。unfinished journal 必须绑定完整 CH343 identity，旧 v2
  缺失 manifest/chunk identity 时只能从已验签的私有 staged package 原子升级；同序
  journal 分叉、mixed legacy copies、替换 ota tool/Python 和同一串口并发运行均拒绝。
- 当前独立验证：C++ host tests `23/23 PASS`；主 Python tests `211/211 PASS`；右板
  独立模型 `29/29 PASS`；GO-1 左板故障注入模型见
  `tests/test_go1_left_fault_injection.py`。
  左右固件构建 PASS。2026-08-06 冻结：左 app `469008` bytes
  SHA `69792BD5…689B5E`；右 app bin `482224` bytes SHA `E6A12B8A…CC5042`；
  `app_main` ELF/`.su` 固定帧 240 B，main stack 12288。共享公钥锚在
  两 app 中逐字节验证一致；内嵌身份均为
  generation 15。右 `handleFrame` 静态栈 80 bytes、rollback restore 288 bytes、DATA
  352 bytes；左最大相关 coordinator 路径 `mutatePairRecord` 2016 bytes。

下一放行步骤：补齐当前 Windows USB1 descriptor 抓包、签名 left-only `.mkota`、
实机只读 bootloader/table/otadata；关闭故障注入覆盖文档中的剩余 LIVE 项；GO-1
书面通过后才允许 generation 15 单次受控左板 OTA。Windows USB1、旧 mouse `move` 和
按 endpoint 独立的有序 FIFO 均不得因 OTA 改造而删除或改义。

## 双板自动更新（MKOT v1，2026-08-03）

当前结论：**生产信任锚、两板 A/B 基础镜像和 generation 3→4 的真实 Linux USB2
双板 OTA 已部署；prepare/activate/confirm 与确认后 controller/XInput/高速 UART 数据面
均实机 PASS。** 普通 application 更新从现在起统一使用可信 Linux 的签名 OTA 编排，
Windows USB1 不提供更新检测或写入入口。默认 mouse `move`、controller overlay 和通用
端点 FIFO 行为没有被删除；只有显式进入 OTA maintenance 后数据面才互斥停用。

### 已实现

- 新增共享 `MKOT v1` 二进制维护协议：24-byte header、2–4 KiB 数据块、CRC32、
  ACK/NAK、sequence/offset、重复块幂等、状态查询、整体 SHA-256、显式
  Prepare/Activate/Confirm/Abort。固件槽只接受角色匹配的 application bin，拒绝
  bootloader、partition table 和 merged image。
- 新增固定 192-byte 签名 manifest，覆盖 role、generation、protocol version、
  peer generation range、partition schema、hardware revision、logical project、镜像
  size/hash 和 signer key id；两板使用 ECDSA P-256 验签。未配置公钥时严格
  fail-close，不允许写 OTA 槽。
- 左板 CH343/UART0 是唯一上位机入口；目标为右板的 MKOT 帧由左板经 UART1
  5 Mbps 转发。维护协议不进入原有 192-byte 实时 bridge FIFO，维护期间普通日志、
  HID report 和 USB Host/Device 数据流均停止。
- 左板实际构建已改为 custom A/B，并启用
  `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=1`：`otadata @ 0xf000`、
  `ota_0 @ 0x20000`、`ota_1 @ 0x1f0000`，两个 app 槽均为 1856 KiB；当前
  flash args 明确把 app 写入 `0x20000`，不再是 single factory。
- 右板保持 `app0 @ 0x10000` / `app1 @ 0x1b0000` 双槽，两个 app 槽均为
  1664 KiB；首次基础镜像包含全 `0xff` 的 8 KiB `otadata @ 0xe000`。当前固定
  Bootloader SHA-256 为
  `E01A2300DE23C8D601E9F2A37684B5DBC83AE37132D5A8833E23520AFD895F92`，
  构建配置和符号均具备 rollback 路径。
- 两板都有 pending-verify 健康门和超时回滚。右板要求 USB Host install/client
  register 正常；左板确认前要求右板运行目标 generation、逻辑 Idle、peer-compatible、
  health-passed 且 UART bridge 正常。
- 左板在 NVS 保存 pair transaction：Prepared、RightActivated、LeftActivated、
  PairConfirmed、RollingBack/MaintenanceRequired。目标不是虚假的跨 Flash 原子提交，
  而是任何已识别的中断组合都保持 fail-close，可由同一 transaction 继续或回退；
  无法证明安全时保持 maintenance，不恢复 USB 透传。
- 激活顺序固定为右板先启动并通过 UART/USB Host 健康检查，再激活左板；确认顺序为
  右板先确认、重新启动并证明目标 generation 健康，再确认左板。`pair-activate` 和
  `pair-confirm` 在发送写命令前先读取实际状态；“右板已激活”及“右板已确认、左板
  尚未确认”两个断电窗口可幂等续跑，不会因重复 Activate/Confirm 卡死。
- 上位机工具 `tools/makcm_ota_tool.py` 已提供 `package/inspect/verify/anchor`、单板
  `status/prepare/activate/confirm/abort` 及单 CH343 入口的
  `pair-prepare/pair-activate/pair-confirm`。Prepare、Activate、Confirm 分别要求独立
  `--allow-ota`、`--allow-activate`、`--allow-confirm`，包 SHA-256 必须显式给出；
  Linux 串口被占用时只报告持有 PID，不会自动杀进程。
- `tools/flash_generic_proxy_pair.ps1` 只用于最后一次人工基础迁移，分别写入两板的
  bootloader、partition table、初始 otadata 和 ota_0 application；必须显式给出
  `-AllowOtaBaseMigration`，保留 COM5 排除和双下载口要求。信任锚缺失、rollback
  证据不符、右板 Bootloader hash 变化或 app 早于信任锚时均在打开串口前拒绝。

### 离线与实机验收证据

- 共享 C++ host tests：`19/19 PASS`，包含 `test_ota_protocol`。
- Python tests：`146/146 PASS`，其中 trusted-Linux 自动编排专项 `18/18 PASS`；右板
  独立协议测试：`4/4 PASS`。
- 左板 ESP-IDF 5.5.5 构建通过：generation 4 app 436064 bytes，OTA 槽剩余约 77%；
  SHA-256 为 `38EC597C0494311B445231AF5DE281D8C5DF2E3EF238D61F770C9290C3AA6117`。
- 右板 PlatformIO/Arduino-ESP32 2.0.16 构建通过：RAM 18.4%，Flash 26.8%；
  generation 4 application bin SHA-256 为
  `E209C54920A59402B00F6978E33C9F68F0E36047514DE25FDD486BD2B0C6BACC`；
  含 A/B 基础段的完整人工刷机镜像 SHA-256 为
  `3931CA90D0DFE55CDA97D331809AB561C450798B8074DADDB3D43D3B13E38CE7`。
- 分区二进制已反解核对；左右 application descriptor 分别为
  `makcm_left_idf` 和 `arduino-lib-builder`，manifest 使用稳定逻辑项目名
  `makcm_left_idf` / `makcm_right_arduino`。
- 生产 signer key id 为 `3dd2425f94e65ad13275fe6d1e2603bd`；固件和 updater 只含
  公钥信任锚，生产私钥仅保存在可信 Linux 的 0600 signing 目录。
- 真实事务 `4208040401` 已按 right→left prepare、right→left activate、right→left
  confirm 完成，`pair_health_confirmed=true`；确认后两板均为 generation 4，左板
  `target_generation=0/state=Idle`。
- 接回 `4131:3519` 后 XInput 9214/9214 读取成功，右摇杆达到完整 int16 端点、两扳机
  达到 255、A 键被观测；累计 1,237,062 帧 gap/duplicate/out-of-order、UART error 和
  endpoint retry/drop/stale 均为 0。完整 `DEBUG_STATUS` 不再破坏 COBS 相位。
- `tools/update_proxy_pair.py` 已部署到可信副机并完成真实 already-current 零写续跑；它
  验签、锁定 CH343 identity、协调 systemd runtime、持久化 transaction/audit，并自动续跑
  prepare/activate/confirm。验签包会先固定到私有事务目录并在 prepare 前重新校验 hash；
  生产公钥文件还必须匹配固定 SHA-256；CH343 identity 会在每个阶段前重验；ACK 后主机
  尚未落盘时，可用板端 PendingConfirm 状态推进滞后的 journal。当前代与目标代的 peer
  range 都必须兼容，runtime scope 也固化在 journal 中。未确认
  journal 禁止被新 release 覆盖，数据面只在 confirm 后恢复。当前副机未安装
  `ai-secondary.service`，本次零写复跑因此没有伪造服务停启。
- 零写复跑审计的工作区副本为
  `diagnostics/ota/gen4_pair_update_noop_20260804.json`，SHA-256
  `9F0758497EB6F4DFA2D5CD0AAF588299758105294388065A0B0AE09B727B3EF5`；事务
  `1624154591` 中没有 prepare/activate/confirm 写阶段事件。

### Direct-stick 审计修复（2026-08-04，源码构建完成、尚未部署）

- EndpointRouter 从 interrupt critical section 改为递归 FreeRTOS task mutex；FIFO、source、
  phase、completion retirement 仍串行化，浮点 compose 与 TinyUSB submit 不再关中断。
  TinyUSB ISR hook 只安排 task-context drain；静态 FIFO 边界测试同步锁定该约束。
- Begin/Leave/OutputMode transition barrier 已双向互斥，禁止 pending barrier 被新命令覆盖；
  completion 与 timeout 两条路径均有 host 回归。
- status buffer 为 1024 bytes，UART 文本上限为 1023 bytes，格式化截断返回明确 NACK；
  `snapshot` 改为纯读 peek，物理 decode reject 新增可见计数。
- `STICK_CAPS` 升为 v2 并新增 `game_cal=0`，status 新增 `decode=`；严格 Python
  parser/harness 已同步，同时保留当前板上 v1 的只读/teardown 解析能力。
- 新左板 app 为 416656 bytes，SHA-256
  `B8B384B0F2E82A01BB4B09629AD4AF82813058F6258E5B69E28FE9253E69F1E9`，IDF 5.5.5
  构建 PASS、OTA 槽剩余 78%。该产物尚未签名/OTA，不能替代当前 generation 4 实机证据；
  部署后仍须重测 stack、status、barrier 和高速 USB cadence。
- 外部 `ai_secondary` Release target 已重建 PASS；外部树未注册 CTest，且其全量
  `ALL_BUILD` 仍被既有的 `soft_active` 测试源码漂移和 GPU postprocess 链接缺口阻断。
  因此这里只声明本次受影响 target 构建通过，不声明整个外部工程测试全绿。

### 仍保留的产品级门槛

1. USB1/USB3 下载口和两个 BOOT 按钮永久保留，仅用于 bootloader、partition、otadata
   迁移或双槽损坏救砖；普通 application 更新不得退回该路径。
2. 继续覆盖坏签名、坏 hash、错 role/schema/generation、丢包、重复块、断串口、杀
   updater，以及 Prepare/每次 Activate/每次 Confirm 前后的逐点断电恢复矩阵。
3. 当前真实非零 generation 更新是手工逐步调用底层 pair CLI；自动编排器的完整状态机
   已测试、并在实机完成目标版本零写续跑。下一次新 generation 发布应保留一份由
   `update_proxy_pair.py` 单命令产生的完整 transaction/audit，作为编排层非模拟 E2E 证据。
4. 右板历史状态曾报告 `reset=7`；当前运行未见重复重启，但长时间 watchdog/断电矩阵仍需
   单独关闭，不能由一次成功 OTA 代替。

## 已落地

- 右板保存并发送原始 Device/Configuration/String/HID/BOS/Qualifier Blob；左板只在能力规划阶段解析，Blob 本身保持权威。
- Blob 按类型、键值、LANGID、offset、完整长度和 CRC16 校验；ProfileBegin 的聚合 CRC 用于跨 Blob 完整性校验。
- 左右板共用 `common/ProxyCapabilityPlanner.h`，在左板安装 USB 前判定 Full/ Limited/ Unsupported，并返回明确拒绝原因。
- 运行期输入/输出使用端点地址元数据，鼠标/XInput 行为位于独立 `ProxyPolicy`，未知设备默认每端点有序 FIFO。
- 右板 UART 发送前也按原始 IN 端点分队列，采用公平 round-robin；队列满时丢新帧并计数，不会用一个端点覆盖另一个端点的旧报告。
- 右板对无数据端点的控制/安全接口不再强制 claim；这类接口仍保留在原始配置和 EP0 路径中，claim 失败不会连带拒绝整个复合父设备。
- 左板 profile 校验失败会回传结构化 `CapabilityResult(Unsupported)`，并同时向 CH343/右板输出 `UNSUPPORTED_PROFILE`/`ESPLOG_PROFILE_REJECT` 原因。
- `StreamStarted` 现在有 `StreamReady` 确认；右板在确认前不开放输入转发，并会重发丢失的启动帧。
- Control response 携带 short-packet/ZLP 标志；BusReset、SetInterface、Suspend/Resume 和端点错误通过通用状态帧传播。
- EP0 转发带 request ID、session 和 250 ms 超时；RAW class driver 返回完整接口组长度，ISR 完成回调关闭，TinyUSB task queue 扩至 64。
- USB suspend 时停止新的端点提交，resume 时按现有 ready/FIFO 状态重新调度；挂起期间的相对量和各端点队列不会被当成传输错误退休。
- 左板对右板 `EndpointInData/EndpointIn` 序列做独立的 uint16 环形跟踪，统计
  缺号帧数/事件、重复、乱序和 session 切换；Profile/Control 重试帧不参与该
  连续序列，避免误报。
- 启用 TinyUSB suspend/resume 事件、FreeRTOS 栈检查、watchpoint 和 UART coredump 诊断。

## 离线验证

以下在 Windows 工作区完成：

```text
PASS: test_bridge_protocol
PASS: test_bridge_sequence
PASS: test_capability_planner       (含 4 IN / 2 OUT 拓扑)
PASS: test_endpoint_tx_phase
PASS: test_hid_mouse_report
PASS: test_mouse_injection_ledger
PASS: test_mouse_injection_rate_gate
PASS: test_profile_blob
PASS: test_profile_store
PASS: test_input_policy
PASS: test_four_in_two_out_profile
PASS: test_generic_profile_routing (非 4131:3519、非 EP83 的描述符派生鼠标)
```

- 左板 ESP-IDF 5.5.5 / esp-tinyusb 2.2.1 / TinyUSB 0.21.0~1 构建通过。
- 右板 PlatformIO `RIGHT` 构建通过。
- 以下为 OTA 改造前的历史构建/刷写记录；当前 OTA 构建以本文上方
  “双板自动更新”的离线验收证据为准：
  - 左板 `MAKCM_ESP32s3_Device_Mouse_Left_IDF/build/makcm_left_idf.bin`
    `D27E59526FE9999C3D1E537B8CC3C85B5D4E14DE0305AC0EE35CEC51DFB04DC1`
    （2026-08-03 direct-stick neutral-only merge/awaitingFresh 语义修复后的未刷写构建；
    通用非鼠标 IN endpoint 仍保持各端点有序 FIFO，未改变 slot、submit、completion
    或 retirement 规则。）
  - 左板完整刷机镜像 `MAKCM_ESP32s3_Device_Mouse_Left_IDF/merged_firmware/Left_MAKCM_MCU_MCU_IDF_GENERIC_PROXY_CURRENT.bin`
    `33FF88DEF3C37A8056663158895E27EFB99B82A98AB8E12969F267097B4FA538`
    （本轮加入挂起/恢复端点门闩及 control-endpoint 解析一致性修复；已在左板 COM3 写入并由 esptool 校验）。
  - 右板 `MAKCM_ESP32s3_HID_Mouse_Right/merged_firmware/Right_MAKCM_MCU_MCU_V1_3.bin`
    `2665D93A99D1682C4EB185874CEE5B57FDF6574D07C4FDDF7BC8BCFE2C9DD786`
    （冷启动 Host 修复版已构建并合并，尚未写入右板）。

## Direct-stick design boundary correction (2026-08-03)

- Direct-stick V1 的产品路径确定为 `SUPPORTED_OVERLAY`：物理 controller persona
  和 generic endpoint FIFO 继续透传，只在已支持 report-format adapter 的 compose
  副本上修改右摇杆。
- Report center/range/sign 按协议格式/adapter 版本管理，不要求每台实体手柄人工
  中心校准或五段单轴扫轴；新增格式的动作采集只是一次性开发取证。
- Game plant 由 synthetic Q15 独立测量完整死区、非线性曲线、方向差异和时间动态，
  分类为 static/dynamic/non-stationary，不能混入 controller adapter profile。
  持续 `move` 必须先变成 requested camera rate 再经过已验证 inverse model；孤立
  `move(1)` 不产生 1-poll 摇杆脉冲。
- 正式 HIP/ADS plant trial 应由 calibration-only controller-field actuator 自动
  press/verify/release；现有 XIM mouse-RMB calibrator 不等同于 direct-controller
  ADS 注入。该 actuator 当前尚未实现，人工切换不能写成可重复 PASS。
- 当前 Linux Secondary 的生产画面是 Windows Host 从 DXGI 原图裁出的中心 ROI
  （现配置 `960x960 -> 640x640`），不能独立证明完整 game plant。目标方案新增互斥
  `calibration-wide` 特别模式：标定时暂停生产 ROI/AI 流，由 Host 将完整 game client
  rect 等比例编码到专用 AimAU/meta 端口；生产路径和 generic endpoint FIFO 不变。
- 正式视频证据必须使用帧内 `session/frame_id/DXGI present QPC`、geometry manifest，
  并对 Host、Secondary、board 三个 monotonic clock 做带漂移和 uncertainty 的映射。
  现有 SEI frame id/Host timestamp 与 RTT offset 是可复用基础，但 wide stream、真实
  `LastPresentTime`、四时间戳/仿射同步和门禁当前均未实现。
- 上述为目标方案修订，不是新增实机 PASS。当前 raw transport、每端点有序 FIFO、
  fair drain、满队列丢新帧和 completion retirement 事实均未改变。

## Transport policy correction (2026-08-02)

The transport no longer enables a VID/PID-specific XInput latest-wins path.
Every non-mouse IN endpoint is an independent ordered FIFO with fair drain
service. Mouse injection remains an optional descriptor-derived policy and does
not change scheduling for the other endpoints. This keeps the revised plan's
generic proxy semantics and avoids silently dropping controller reports.

The current protocol also carries `ControlOutData` with a complete request
identity and carries `SetConfiguration` lifecycle markers.  The left side
requires the right-side capability result and compares it with its own planner
before installing TinyUSB.  BOS blobs preserve their complete `wTotalLength`
chain, and valid non-one configuration values are retained unchanged.

The profile handshake now also retries an incomplete same-session receive:
when a `ProfileReady`/blob frame is lost, a retransmitted `ProfileBegin`
restarts the left-side append transaction instead of leaving it permanently in
`ReceivingProfile` with an incomplete offset bitmap.

The generic HID path now keeps the legacy report-length field per primary
interface instead of summing all composite reports.  The left validator also
uses the report grammar to identify a Mouse application collection, so
non-Boot HID mice (subclass/protocol zero) can receive descriptor-derived
injection without changing their raw descriptor.  Profile validation no longer
rejects a complete per-interface Blob set because of the old aggregate field.

The successful mouse/controller switching run is therefore not explained by
an `EP83` transport switch.  `ProxyPolicy` obtains the mouse address from the
parsed HID Mouse application collection, and `EndpointRouter` sends every
other endpoint through its own FIFO.  There is one deliberate device-specific
layer: `InputPolicy` recognizes `4131:3519` (and the older DSE profile) to
decode the two trigger/button bits used by the optional Secondary arm/mapping
feature.  That policy is not used to select, schedule, or forward USB
endpoints.  `test_generic_profile_routing` changes both VID/PID and the mouse
endpoint and still passes, which is the offline guard against the transport
being hard-coded to `4131:3519`/EP83.

On a profile change the left side now holds D+ detached for 50 ms before
uninstalling TinyUSB.  This prevents Windows from treating a newly attached
keyboard/mouse as a continuation of the previous controller composite device.

`DeviceGone` is acknowledged even when the left side has no active profile;
this covers a right-side hot-plug race with a left reboot and prevents the host
from treating a missing teardown ACK as a fatal bridge failure.

The offline suite now also covers the optional-policy boundary:
`test_input_policy` verifies that unknown profiles never receive controller
mapping or fallback mouse layout logic.
`test_four_in_two_out_profile` validates the complete 4-IN/2-OUT raw profile,
including a control-only security interface, CRC and capability gate together.
`test_generic_profile_routing` changes both VID/PID and the mouse endpoint to
prove that `ProxyPolicy` follows the HID report grammar rather than a
`4131:3519`/`EP83` switch.  The remaining VID/PID-specific code is explicitly
limited to optional trigger/button interpretation; it is not used to choose
or forward raw USB endpoints.

## 高速并行观测

`tests/windows_raw_input_harness.py` 是 Windows 类驱动层的并行观测器：它
不打开会被 `xusb22`/`mouhid` 占用的 HID 文件，而是用 `XInputGetState` 加
隐藏窗口的 Raw Input，同时统计 XInput poll/packet-change、目标鼠标
`VID/PID` 的 Raw Input 报告、相对位移、最大报告间隙和 Windows 错误。
这使 Secondary 在另一台 Linux 副机以 1000 Hz 发送 `km.move` 时，仍可在
主机侧验证手柄是否持续前进；没有物理鼠标活动的运行只能算连接探测，不能
算吞吐通过。

建议压力门槛：

```powershell
python tests\windows_raw_input_harness.py --vid 0x4131 --pid 0x3519 `
  --seconds 600 --out diagnostics\windows_raw_input_pressure.json `
  --require-target-activity --require-xinput-change --min-target-events 1000 `
  --max-target-gap-ms 20
```

先启动该命令，再让 Secondary 以 `report_hz=1000` 连续发 `km.move`，同时
移动手柄对应的物理鼠标并让 XInput 状态持续变化。报告中的
`target_events`、`target_dx/target_dy`、`max_target_gap_ms` 与 XInput
计数必须非零且无 `failures`。这项观测不能替代左板/右板的
 `LEFT_STATUS`/`DEBUG_STATUS`；要宣称“无丢帧”，还需在同一窗口前后采样
 板上 drop/overwrite/txerr/CRC/overflow 计数并比对增量。

`LEFT_STATUS` 中 `seq=frames/gap_frames/gap_events/duplicates/out_of_order/session_changes`
是右板输入 EndpointIn 序列的板上观测值；正常连续压力窗口应保持后五项为
`0/0/0/0/0`，session 切换只允许最后一项增加。

新增 `tests/compare_status_window.py` 可直接比较压力前后的 CH343 文本日志，
自动展开 `ep/ctrl/sec/inj/btn/seq` 字段，并对左板 drop/stale/CRC/序列错误、
Secondary overflow 以及右板 txerr/overwrite/rxerr/cleanerr 做增量判定；
它不会把历史累计值误判成当前窗口错误。

The first post-flash run with real target activity was recorded as
`diagnostics/windows_raw_input_pressure_20260802_182517.json`: XInput was
`1975/1975` with 1974 packet changes and no errors, while the target Raw Input
mouse delivered 561 reports.  It is evidence that Secondary -> EP83 -> Windows
Raw Input and XInput coexist, but it is not a stability pass: the largest
target gap was 1060.032 ms.  A strict 30-second gate was then run with
`--min-target-events 100 --max-target-gap-ms 20` and recorded as
`diagnostics/windows_raw_input_pressure_strict_20260802_183038.json`; it
failed with only 72 target reports and an 18697.929 ms maximum gap, while
XInput remained `1974/1974` and error-free.  The matching Secondary log window
shows repeated `DISARMED`/`ARMED` transitions and `makcu fail=0`, so this run
demonstrates source-side arm/output intermittency, not EP81 starving EP83.

For a firmware stability sign-off, the next run must use a deterministic,
continuously armed source (a fixed-rate `km.move` generator or a held physical
arm with a known report cadence), and must collect the Windows harness plus
before/after `LEFT_STATUS`/`RIGHT:ESPLOG_STATUS` counters in the same window.
The pass gate is target activity, XInput success/packet changes, target max gap
<= 20 ms, and zero current-window board drops/overwrites/retries/sequence
errors.  A run driven by intermittent inference or arm transitions is recorded
as a source test, not as a proxy throughput result.

The deterministic source is now checked in as
`tests/makcu_move_stress.py`.  It uses an exclusive serial open, refuses to
run while `ai_secondary` owns `/dev/ttyACM0`, emits fixed-rate `km.move` lines,
records its own write rate/gaps, and sends a final zero move.  This makes the
simple test reproducible without changing the firmware or mixing inference
latency into the USB result.  The Windows observer is still required for the
actual EP83/EP81 acceptance; the source tool's own pass only says that the
generator maintained its requested cadence.

The Raw Input observer's `RAWMOUSE` union layout was corrected on 2026-08-02:
the previous declaration shifted `lLastX/lLastY` by four bytes and falsely
reported zero motion.  The corrected 10-second direct-source run
`diagnostics/windows_raw_input_pressure_dx100_fixed_20260802_185507.json`
passed the strict gate with 5,000 `km.move(100,0)` commands, 448 target
reports, `target_dx=500000`, XInput `650/650`, and max target gap 18.034 ms.

A longer 30-second direct-source run is recorded in
`diagnostics/pressure_direct_30s_20260802_185602.md` and
`diagnostics/windows_raw_input_pressure_30s_fixed_20260802_185602.json`.
The board-side counters are clean in the same session: 30,001 accepted
injections, 30,002 Secondary commands, zero sequence gaps/duplicates/out of
order, and no new endpoint retry/drop/stale or UART parse/overflow errors.
Windows observed 2,606,300 counts concurrently with XInput `1946/1946`; its
only strict failure was a 29.249 ms Raw Input delivery gap.  This is treated
as a host delivery/coalescing observation because the board's 1 kHz source and
completion counters remained loss-free; the 10-second run is the strict <=20
ms Windows-gap pass.

Both OUT class-driver paths were also exercised by
`tests/windows_out_harness.py`:
`diagnostics/windows_out_pressure_20260802_185945.json` records 500/500
successful `XInputSetState` vibration calls and 500/500 successful 65-byte
vendor-HID output reports at 50 Hz, with no Windows errors.  This is an
application-layer OUT acceptance result; a raw USB capture would still be
needed to claim packet-level OUT timing.

After the pressure and OUT runs, the read-only topology checks were repeated:
`diagnostics/windows_hid_snapshot_20260802_190051.json` still lists 5 HID
collections and `diagnostics/windows_usb_hub_snapshot_20260802_190051.json`
still reports the 146-byte, 5-interface, EP81/01 + EP82 + EP83 + EP84/02
configuration.

## 尚未宣称完成

代码、离线测试和两块板修复镜像的写入已经完成；最新左板镜像已经重新枚举并通过
EP81/EP83 并行输入与两路应用层 OUT 测试。这里的“尚未宣称完成”仅表示完整 P4
仍缺连续热插拔、USB reset、PC 睡眠/唤醒，以及 EP82/EP84 同时产生真实流量时的
四路 IN 并行证据；不能用已完成的两路活动端点测试代替这些剩余项目。

## 硬件动作与当前证据（2026-08-02）

- 左板已通过 COM3 写入当前修复镜像
  `Left_MAKCM_MCU_MCU_IDF_GENERIC_PROXY_CURRENT.bin`；esptool 返回
  `Hash of data verified`，随后硬复位。镜像 SHA-256：
  `CBFC36047333813C8115BE53AF29DE1867807DCC7C1B79F3BA005DA727140DDF`。
- 左板此前的序列计数版本已构建并合并为
  `5B7FB89FF409440E40FFD6C890ADC254E525F958D4E05347195F824FC32B2EB3`，
  已在 COM3 下载态写入，esptool 返回 `Hash of data verified`，芯片 MAC
  `10:51:DB:43:46:8C`，随后硬复位。本轮新增挂起/恢复门闩的镜像为
  `A6FB434E18DE77C3B322C28FE5D1B0BB9656B4CC209887105FAA487EA6B1C597`，
  仅完成离线构建，未改变当前板上运行版本。
- 本轮最新左板镜像已通过 COM3 写入：
  `Left_MAKCM_MCU_MCU_IDF_GENERIC_PROXY_CURRENT.bin`，芯片 MAC
  `10:51:DB:43:46:8C`，镜像 SHA-256 为
  `38CF6431747E2944ABC11E871BA1AC2C90F392166D9F60708E768B62A3BD5B35`，
  esptool 返回 `Hash of data verified`。硬复位后当前 Windows 未重新看到
  COM/`VID_4131:PID_3519`，所以这次只算“写入通过”，不算运行期枚举通过；
  记录见 `diagnostics/windows_hid_snapshot_post_left_flash.json`。
- 随后针对 control-endpoint 解析一致性修复重新合并的左板镜像也已通过
  COM3 写入并校验，芯片 MAC 仍为 `10:51:DB:43:46:8C`，镜像 SHA-256 为
  `33FF88DEF3C37A8056663158895E27EFB99B82A98AB8E12969F267097B4FA538`。
  接回运行线后代理父设备已重新枚举；最新快照
  `diagnostics/windows_hid_snapshot_20260802_211849.json` 打开 5 个 HID 集合，
  `diagnostics/windows_usb_hub_snapshot_20260802_211849.json` 读取到 146 B、5 接口、
  EP81/01 + EP82 + EP83 + EP84/02 的完整 4-IN/2-OUT 拓扑。
- 最新镜像的 5 秒连接基线
  `diagnostics/windows_raw_input_baseline_20260802_211901.json` 中 XInput 为
  `324/324`、packet change `323`、最大 XInput 间隔 16.598 ms，无错误。
- 最新镜像的确定性并行窗口记录在
  `diagnostics/makcu_move_stress_post_flash_20260802_212224.json` 与
  `diagnostics/windows_raw_input_post_flash_20260802_212224.json`：Secondary 主机
  以 1 kHz 发送 9646 条 `km.move(100,0)`，source 最大写入间隔 2.609 ms；Windows
  同时观察到 XInput `778/778`、777 次 packet change、1013 个目标鼠标 Raw Input
  事件和 `target_dx=964000`。同一窗口的板端计数显示 sequence 缺号/重复/乱序均为
  0，endpoint retry/drop/stale 无新增，Secondary parse/tx/overflow 为 0，9647 次注入
  接受（含最终归零）且拒绝为 0。Windows Raw Input 最大交付间隔为 31.062 ms，
  8 次超过 20 ms、0 次超过 50 ms；后续报告携带 1500–1700 的合并位移，因此该项
  证明板端无丢帧和 EP81/EP83 可并行，但不冒充严格的 Windows `<=20 ms` 交付门通过。
- 最新镜像的双 OUT 应用层测试
  `diagnostics/windows_out_post_flash_20260802_212341.json` 在 50 Hz、5 秒窗口内完成
  XInput 振动 `250/250` 和 65 B 厂商 HID OUT `250/250`，无错误。
- 尝试仅重启代理父节点
  `USB\\VID_4131&PID_3519\\123456789012` 以覆盖 Windows 重新配置路径时，
  `pnputil /restart-device` 返回 `Access is denied`；父设备及子节点保持在线，未发生
  reset。因此这次操作明确不计为 USB reset 验收，仍需物理动作或管理员权限。
- 随后的“所有端口同时拔插”不是单独下游热插拔，而是整机冷启动。重新接回后
  Windows 恢复了 5 个 HID 集合和 146 B 拓扑，Secondary 也自动重连
  `/dev/ttyACM0`；但 `diagnostics/windows_raw_input_cold_replug_20260802_213436.json`
  与 `diagnostics/windows_raw_input_cold_replug_recheck_20260802_2138.json` 中
  XInput 虽分别成功 `323/323`、`195/195`，packet number 始终为 0。左板状态为
  `Streaming`、6 个端点已开但 `seq=0`，右板诊断显示 `connected=0/state=Empty/
  session=0/input=0` 并在 `ESPLOG_NEW_DEVICE:1` 附近再次出现启动标识。由此将故障
  定位到右板冷启动 Host 枚举路径，而不是 Windows 或左板 StreamReady 门闩。
- 右板冷启动修复移除了 USB client callback 内嵌套的 `std::stringstream`/INFO
  原始字节格式化，改为有界 DEBUG 分块；`usbClientTask` 栈从 4096 增至 8192，
  并在启动/`ESPLOG_STATUS` 中加入 reset reason、heap 和 client stack high-water。
  修复版 PlatformIO 构建通过，完整镜像 SHA-256 为
  `2665D93A99D1682C4EB185874CEE5B57FDF6574D07C4FDDF7BC8BCFE2C9DD786`，
  尚待写入右板后复测整机冷启动。
- 随后左板运行态已重新出现为 ESP32-S3 原生 USB 串口（COM6，VID/PID
  `303A:0009`），说明芯片已启动；但此时 `VID_4131:3519` 代理父设备仍未出现，
  且原生 USB 的 MI_02 节点为错误状态。COM6 不是接收 CH343/UART0
  `LEFT_STATUS` 的运行诊断口，因此不能据此判定左板 profile 解析失败；需要把右板、
  CH343 桥和下游手柄运行线同时接回后再取 `LEFT_STATUS`/`RIGHT:ESPLOG_STATUS`。
- 右板已通过 COM6 写入当前修复版 `Right_MAKCM_MCU_MCU_V1_3.bin`；esptool 返回
  `Hash of data verified`，随后硬复位。镜像 SHA-256：
  `5C1E80C4187ADD5C8AAC7CB5FE92119762ADD6BD3B793E58A707FDF8446BEAAB`。
- 写入后两块下载口均在硬复位后退出；这是下载口消失，不代表运行期枚举已经验收。
- 右板最新镜像本轮再次通过 COM6 写入并校验后，COM6 正常退出；随后
  在运行线接回状态下重新做了冷枚举快照。
- Windows 当前能看到 `VID_4131:PID_3519` 父设备以及 XInput、键盘、鼠标和厂商 HID 子设备；
  PnP 节点状态均为 OK，唯一的 `Xbox Security Method 3` 子接口仍为历史上
  的 Code 28，但不影响父设备和其余逻辑集合。
- libusb 在 Windows 原生 HID/XInput 驱动占用下曾读到 41 字节的错误视图；
  通过根 Hub 的只读 `IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION` 重新读取
  当前物理端口后，权威快照
  `diagnostics/windows_usb_hub_snapshot_20260802_181947.json` 显示：Hub 端口 9、
  `wTotalLength=146`、5 个接口以及完整的 EP81/01、EP82、EP83、EP84/02。
  这与目标 4-IN/2-OUT 配置一致，之前的“单接口 persona”结论已撤销。
- Windows HID parser 快照
  `diagnostics/windows_hid_snapshot_20260802_181947.json` 还成功打开了 5 个
  逻辑集合，包括键盘/消费者、鼠标（Usage 0x02，7B）、厂商 HID（65B
  IN/OUT）和游戏手柄（Usage 0x05，15B），且 `XInputGetState(0)` 返回成功；
  因此“手柄 + 鼠标”同时存在是真实的，但鼠标仍是独立 HID 逻辑功能，不是
  XInput 报告字段。
- 5 秒 Windows Raw Input/XInput 基线
  `diagnostics/windows_raw_input_harness_20260802_180031.json` 中
  `XInputGetState` 成功 `328/328`、packet change `327`、无 Raw Input/Win32
  错误；该次没有移动目标鼠标，所以 `target_events=0`，明确标为连接基线，
  不冒充高速吞吐验收（序列计数版本刷写前）。
- 随后 20 秒压力窗口
  `diagnostics/windows_raw_input_pressure_20260802_180355.json` 中
  `XInputGetState` 成功 `1310/1310`、packet change `1309`、无错误；由于
  该窗口没有收到目标 `VID_4131&PID_3519` 的 Raw Input（`target_events=0`），
  按门槛明确记为 `no_target_mouse_activity`，不是高速稳定性通过（序列计数版本刷写前）。要完成
  该项，必须让 Secondary 的 1000 Hz `km.move` 或物理鼠标实际经过 EP83，
  并在同一窗口采到非零目标事件和板上计数增量。
- 新左板刷写后的 10 秒 Windows 基线
  `diagnostics/windows_raw_input_harness_20260802_182000.json` 中
  `XInputGetState` 成功 `652/652`、packet change `651`、无 Raw Input/Win32
  错误；目标鼠标仍未产生活动（`target_events=0`），因此只证明新镜像的
  枚举与 XInput 并行链路，没有冒充 EP83 高速通过。
- 历史快照 `diagnostics/proxy_snapshot_20260802_165408.json` 曾只读取到 41 B
  单接口视图；该结论已被根 Hub 读取的 146 B 权威快照以及最新 5 接口枚举推翻，
  不再代表当前运行态。
- 最新 5 秒 libusb 压力探测列出 EP81/82/83/84 与 EP01/02，但只有 EP81
  能读到报告，其他端点被 Windows 原生驱动返回 I/O error；这属于测试方法的
  驱动占用限制，不再作为描述符失败证据。4-IN/2-OUT 的持续数据、OUT 写入、
  热插拔、睡眠唤醒和 panic/CRC/drop 仍需 USBPcap/WinUSB 或应用层 XInput+HID
  读写完成最终验收。

因此仍未宣称完整 P4 完成：最新镜像的冷枚举、描述符/拓扑、EP81+EP83 并行输入、
两路 OUT 和板端无新增 CRC/drop/sequence 错误已有证据；剩余的是连续热插拔、明确的
USB reset、PC 睡眠/唤醒，以及 EP82/EP84 也持续产生真实输入时的四路 IN 同窗压力。
## 2026-08-02 右板冷启动修复刷写与复测

- 修复后的右板完整镜像已通过 COM6 写入；芯片 MAC 为
  `10:51:DB:43:46:90`，镜像 SHA-256 为
  `2665D93A99D1682C4EB185874CEE5B57FDF6574D07C4FDDF7BC8BCFE2C9DD786`，
  esptool 返回 `Hash of data verified`。
- 恢复全部运行接线后，Windows 重新枚举 `VID_4131:PID_3519`。快照
  `diagnostics/windows_hid_snapshot_right_coldfix_20260802_215819.json`
  打开 5 个 HID collection；
  `diagnostics/windows_usb_hub_snapshot_right_coldfix_20260802_215819.json`
  读到 Hub 端口 9、146 字节配置描述符。
- 冷启动后的 6 秒连接实测记录在
  `diagnostics/windows_raw_input_right_coldfix_20260802_215819.json`：
  `XInputGetState` 成功 `389/389`，packet change `388`，最大 packet-change
  间隔 `16.521 ms`，无失败；最后 packet number 为 `473132`。这直接修复了
  旧镜像冷启动后 XInput 虽能打开但 packet number 始终为 0 的故障。
- Secondary 已重新连接 `/dev/ttyACM0` 并保持运行；日志中的按钮流
  `rx_age=0ms`。本轮没有产生目标鼠标位移，因此该 6 秒记录仅用于冷启动
  XInput/枚举验收，不冒充 EP83 压力测试。
- 已完成一次仅针对最右侧下游手柄线的热拔插闭环，期间左板上行线和左右板
  通信线均未动。拔出态记录
  `diagnostics/windows_raw_input_downstream_unplug_20260802_220235.json` 中
  `XInputGetState` 连续 `0/323`，错误码为 `1167`（设备未连接），且
  `VID_4131:PID_3519` 父设备从 Windows 消失，证明旧 session 已结束。
- 插回后父设备自动重新枚举；
  `diagnostics/windows_hid_snapshot_downstream_replug_20260802_220410.json`
  再次打开 5 个 HID collection，
  `diagnostics/windows_usb_hub_snapshot_downstream_replug_20260802_220410.json`
  再次读到 146 字节配置描述符。
  `diagnostics/windows_raw_input_downstream_replug_20260802_220410.json` 中
  XInput 成功 `388/388`、packet change `387`、最大 packet-change 间隔
  `16.171 ms`，最后 packet number 为 `41777`。这证明下游独立拔插可以建立
  新会话并恢复输入，不需要拔插左板上行 USB。
- 连续第 2 轮下游拔出记录为
  `diagnostics/windows_raw_input_downstream_unplug_round2_20260802_220532.json`：
  父设备节点为 0，XInput `0/259`，错误码 `1167`。插回后的
  `diagnostics/windows_raw_input_downstream_replug_round2_20260802_220618.json`
  为 XInput `325/325`、packet change `324`、最大间隔 `16.412 ms`；对应 HID
  和 Hub 快照再次确认 5 个 collection 与 146 字节配置。
- 连续第 3 轮下游拔出记录为
  `diagnostics/windows_raw_input_downstream_unplug_round3_20260802_224021.json`：
  父设备节点为 0，XInput `0/259`，错误码 `1167`。插回后的
  `diagnostics/windows_raw_input_downstream_replug_round3_20260802_224136.json`
  为 XInput `387/387`、packet change `386`、最大间隔 `16.630 ms`；对应 HID
  和 Hub 快照仍为 5 个 collection 与 146 字节配置。
- 因此三轮连续“只拔插最右侧下游手柄线”验收通过：每轮拔出都明确结束旧
  session，每轮插回都无需动左板上行线即可自动建立新 session 并恢复 XInput。
- 尚待执行：明确的 USB bus reset、PC 睡眠/唤醒，以及 EP82/EP84 也产生真实
  输入时的四路 IN 同窗压力。
## 2026-08-03 DualSense Controller-only Adapter

- 新增 `common/DualSenseControllerAdapter.h`，对标准 DualSense
  `054C:0CE6` 和 DualSense Edge `054C:0DF2` 启用。普通设备和
  `4131:3519` 仍走原有 Generic Exact Proxy，不经过该转换器。
- 转换器只接受与 `wTotalLength` 完全一致的完整下游 Configuration Blob；它从
  完整配置中保留 HID、Alternate Setting 0、Interrupt、MPS <= 64 的控制器接口，
  删除 Audio、非零 Alternate Setting 和 Isochronous 端点，并重写
  `wTotalLength`、`bNumInterfaces`、接口号和必要的端点地址。
- 右板保存虚拟接口/端点到物理接口/端点的双向映射。下游 interface claim、
  Interrupt IN、OUT、通用 EP0 以及专用 HID Feature GET/SET 均使用物理地址；
  对已经从上游 persona 删除的接口或端点请求明确返回 STALL。
- 仓库内 `PS5_Controller.txt` 的配置头声明 227 字节，但实际 Raw Bytes 只有
  195 字节，缺少最后 32 字节 HID interface group。因此它只作为“截断必须拒绝”
  的证据，不能用来硬编码或声称完成 DS 描述符复刻；实机右板必须抓到完整
  227 字节后才允许转换。
- 新增 `tests/test_dualsense_adapter.cpp`，覆盖 Audio/ISO/alternate 过滤、HID
  保留、接口/端点双向映射、转换后 capability pass 和截断 blob 拒绝。完整
  `tests/run_host_tests.py` 共 13 项测试全部通过。
- 右板 PlatformIO `RIGHT` 构建通过，RAM 12.0%，Flash 21.7%。当前完整刷写镜像
  `MAKCM_ESP32s3_HID_Mouse_Right/merged_firmware/Right_MAKCM_MCU_MCU_V1_3.bin`
  为 435744 字节，SHA-256：
  `A8C10F83AD36D5B22696CCEA9667BD306DE2BEF5BBF6A9321C1263B866EF9B2D`。
- 左板使用固定 IDF 5.5.5、esp_tinyusb 2.2.1、TinyUSB 0.21.0~1 的 canonical
  build 通过，说明共享协议/测试新增没有破坏左板构建。
- 第一版仅匹配 `054C:0CE6` 的镜像
  `95D633FE618793F3EAA0FAEA33BFDF1AB2353294EE43FEB3C6309ADD3DC89189`
  曾通过 COM6 写入右板，芯片 MAC 为 `10:51:DB:43:46:90`。现场实际设备为
  `054C:0DF2`，所以适配器没有启动；`DEBUG_STATUS` 证明右板已经完整抓到
  `cfg=227/227`、IF3 HID、EP84/EP03，但以
  `reason=9 (alternate_setting)` 拒绝，左板保持 `profile=0/install=0`。
- 已将 `054C:0DF2` 纳入同一转换器，重新完成 13/13 主机测试与右板构建；
  `A8C10F83AD36D5B22696CCEA9667BD306DE2BEF5BBF6A9321C1263B866EF9B2D`
  镜像已通过 COM6 写入右板，芯片 MAC `10:51:DB:43:46:90`，esptool 返回
  `Hash of data verified`，硬复位后 COM6 正常退出。
- 当前状态是“根因定位、修复、离线测试、构建和右板写入通过，等待恢复运行接线
  后完成 DSE 实机验收”。该
  persona 明确不提供 DualSense USB 扬声器、麦克风或其他 Isochronous 音频功能，
  也不等于完整透明复制原始 DualSense USB 配置。恢复运行接线并接入 DS 后，必须
  先确认右板抓到完整 227 字节配置，再验证 Sony HID 枚举、输入、OUT/Feature 和
  下游热拔插。
