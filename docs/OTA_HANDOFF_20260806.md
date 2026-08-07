# MAKCM 双板 OTA 交接说明（2026-08-06）

本文档面向接力的下一个 agent。它记录**当前经过验证的实机状态**、本轮的代码改动、
以及几条被实机推翻的错误结论。请优先相信本文的"已验证事实"一节，其余历史文档
（尤其是 `MAKCU_USBPROXY_FS_IMPLEMENTATION_STATUS.md` 中 generation 3→4 的记录）
只作为历史证据，不代表当前放行状态。

---

## 1. 系统总设计

### 1.1 硬件与信任边界

```text
Windows USB1 ──► 左板 (ESP32-S3, ESP-IDF 5.5.5, USB Device)
                   │  UART0 / CH343  ◄── Linux USB2 (可信更新入口)
                   │  UART1 5 Mbps
                   ▼
                 右板 (ESP32-S3, Arduino/IDF 4.4.7, USB Host)
                   │  USB3
                   ▼
                 手柄 / 鼠标
```

不可破坏的边界：

- **Windows USB1 永远没有 OTA 探测或写入入口**。它只表现为透传/转换器数据面。
- 更新只能从可信 Linux 经 CH343 → 左板 UART0 发起；目标为右板的帧由左板经 UART1 转发。
- 旧 mouse `move` / pass-through 不得删除；通用端点保持按 endpoint 独立的有序 FIFO。
- USB1/USB3 的 ROM 下载口和 BOOT 按钮永久保留，仅用于 bootloader/分区/双槽损坏救砖。
- 生产固件不得常驻危险探针（早期 gen10/gen11 在 `app_main` 早期擦写 Flash，自诱发过 panic）。

### 1.2 四层职责与单一事实源

这是本项目最重要的架构约定，之前多轮失败都源于层间耦合：

| 层 | 事实源 | 只能做 | 不能做 |
|---|---|---|---|
| 本板 image lifecycle | running/boot partition、`esp_ota_get_state_partition()`、本板 Snapshot V2 | 判定本槽生死、mark-valid、回滚 | 依赖 pair/host 状态决定本槽存亡 |
| Pair transaction | 左板 NVS PairRecord（内容必须是两板 Status V2 精确事实的副本） | 观察、校验顺序、持久化 intent、决定数据面是否释放 | 猜测某板状态、为未接受 MANIFEST 的板伪造 tombstone、改本板 image state |
| Host journal | 只记录 host intent 和已观察到的板端事实 | 证明发了什么命令、观察到什么 Status V2 | 单独证明 boot selector/VALID/Confirm 是否落盘 |
| UART 维护门 | 单一维护门 | 启动即 fail-close，证明 Idle 或可信 terminal 后才放行数据面 | `begin()` 失败却照常启动 USB Host/bridge |

`LOCAL_VALIDATED` 之后，host 失联、手柄缺失、pair 超时都只能保持维护，**不得再杀本板镜像**。

### 1.3 更新流程

```text
右 Prepare → 左 Prepare → 右 Activate → 右重启并证明健康
  → 左 Activate → 右 Confirm → 左 Confirm → 双板回 Idle，数据面恢复
```

右板先行。两板独立 Flash，不宣称跨板原子提交；目标是**任意断电点都能恢复到兼容组合
或 fail-close 维护态**，绝不留下永久新旧混搭。

---

## 2. 已验证的当前实机状态

### 2.1 两板

| 项 | 左板 | 右板 |
|---|---|---|
| generation | 16 | 16 |
| app_version | makcm-g16 | makcm-g16 |
| 状态 | Idle，`local_boot_validated=true`，flags=204 | Idle（USB Host `registered=1`） |
| 当前运行 image SHA-256 | `cac27add077416e2a40a7c1bf939c6248772c7408690a1a9c2491dfbcc848db6` (479088 B) | `da1c483c5ec6448f0e0160bae94bdf710c8781da292d44c5d23d73866f979007` (482480 B) |
| 刷入方式 | COM3 直刷 `ota_0 @ 0x20000` | COM6 直刷合并镜像 `@ 0x0`（`df7df988c98ecc39b2e42ee8681952b131bbfa9819c593d5113313a1a244ed49`, 548016 B） |

左板**板上运行的**镜像仍是带诊断探针 `PROBE_RIGHT_HELLO` 的构建（见 4.1）。
探针已于 2026-08-07 从源码删除，工作树现在构建的是干净生产镜像
`c381ce8e55bc0f2c78c011090783652fc8f87524e2ed1005b05eb2a469a359bd` (469488 B)，
**尚未刷入**。左板下一次落盘（COM3 直刷或 gen17 OTA）之后本行才作废。

### 2.2 链路与数据面

以下均为本轮实测通过：

- CH343 文本面：`km.version()` → `MAKCM native-left`；`LEFT_STATUS` 正常。
- 左板 MKOT HELLO：Idle / active=16 / local_boot_validated。
- UART1 双向：`DEBUG_STATUS` → `RIGHT:ESPLOG_STATUS …`；`READY` → `RIGHT:USB_ISNULL`；
  左板 `text=` 计数持续增长。
- **手柄透传实测通过**（用户确认）。
- 左板断电重启后仍为 gen16 + local_boot_validated。

### 2.3 自动化验证

- 主 Python 测试 `224/224 PASS`
- 右板独立模型测试 `29/29 PASS`
- C++ host 测试全部 PASS（`python tests/run_host_tests.py`）
- 左右固件构建均 PASS

### 2.4 尚未做过的事（重要）

**完整的 V2 协议 OTA 从未在硬件上跑通过。** 迄今所有实机成功要么是 gen5→genN 的
V1 桥，要么是 COM3/COM6 直刷。这是 GO-1 唯一还欠的证据。

---

## 3. 本轮代码改动

### 3.1 右板 OTA 接收器（`MAKCM_ESP32s3_HID_Mouse_Right/src/ota_receiver.cpp`）

1. **P0：空 NVS 初始化失败**。`loadSnapshotV2()` 的初始化分支只设了 `snapshot_.role`，
   没有同步 `pendingSnapshot_`，而 `commitPendingSnapshotV2()` 校验的是后者。默认
   `role` 是 `Host`，所以**空 NVS 的第一次提交必然被自己拒绝** → 每次上电都
   `Failed` + `FlashWriteFailed` + 维护态。修复是补 `pendingSnapshot_ = snapshot_;`。
   实机验证：`reason` 从 10 变 0。
   *这个 bug 让"擦 NVS 救砖"变成了"擦得越干净锁得越死"。*
2. **P0-X1：目标固件首启恢复分支不可达**。`ActivateIntent/BootSelected` 开头用
   `identity.generation <= kCurrentGeneration` 直接 return，导致后面
   `generation == kCurrentGeneration` 的新固件首启分支永远走不到。改为按当前运行槽分支：
   `runningPrevious` 要求 target 代更大，`runningTarget` 要求相等。
3. **P0-X2：回滚墓碑 generation 判定错误**。统一要求 `generation == kCurrentGeneration`，
   但回滚后运行的是 previous 固件。拆开：Confirmed 比 `generation`，
   Aborted/RolledBack 比 `previous_generation`。
4. `RollbackIntent`/`RolledBack` 恢复：必须证明 `running==boot==previous slot` +
   镜像 SHA + `VALID` 才持久化 `RolledBack`，否则永久维护。
5. Abort 改为结果导向：`abortUpdate()` 返回 bool；失败不得 `Idle`、不得释放维护、不得重启。
   `finalizeCommandResponse()` 失败会撤销 release/restart。
6. 删除逐 DATA 块的 NVS commit。重启后本来就必须从 offset 0 重发，逐块写没有
   resumability 价值，只增加 Flash 磨损和失败窗口。
7. DATA 重放身份从"CRC32 + offset"改为绑定 `sequence` + `SHA-256`。
8. `begin()` 任何初始化失败都置维护态；`esp_tasks.cpp` 据此预留 USB Host，
   不再"更新服务不可用但普通桥照常跑"。

### 3.2 右板 USB Host（`commands.cpp` / `esp_usb_host.cpp` / `EspUsbHost.h`）

- 新增 `HostState::OtaReserved`，`hostState` 改为 `std::atomic`。OTA bootstrap 和
  NEW_DEV 热插拔现在争用**同一个 `Empty` CAS**，关闭了 TOCTOU。
- 维护态下仍解析文本 `OTA_MAINT_BEGIN`（对称于左板 Idle HELLO 修复），
  避免 NVS 点亮维护后表现为"UART 死寂"。
- 所有 bootstrap 拒绝路径都必须回 `OTA_UNAVAILABLE`，不再静默超时。

### 3.3 左板（`main/app_main.cpp` / `components/makcm_core/`）

1. **P0：`begin()` 死代码**。之前为 gen5→gen16 桥接加的 `if (have_record)` 块在
   **两条路径上都 `return true`**，把它下面整段恢复逻辑变成死代码——其中就包含
   `PendingConfirm + VALID → 恢复 PendingConfirm`。后果是 mark-valid 成功后
   `saveRecord()` 写下 `state=PendingConfirm`，下次上电因 image 已 `VALID` 而
   `pending_verify=false`，落到兜底 `Fault`，**断一次电就丢透传**。
   修法是删掉这个拦截块——下面的 per-state dispatch 本来就是完整的桥。
2. `secondary.cpp`：去掉 `ota_maintenance_` 对 MKOT 解码器的门控，按帧边界识别 magic。
   Idle 板现在也能收 HELLO/ENTER。
   **隐性契约：CH343 文本面上 `M` 前缀归 MKOT 所有**（`kMagic = "MKOT"`）。当前所有
   命令都安全（`km.*` 小写、`LEFT_*`/`READY`/`DEBUG_*`/`OTA_*` 均非 M 开头）。
3. pair reducer：`left Prepared + right LocalValidated` 是合法的 right-first 中间态，
   不再映射为 `MaintenanceRequired`。左板 Activate 增加 right-local-validated 守卫。
4. 单边回滚退休：右板已 Prepare 但左板从未接受 MANIFEST 时，只有右板精确证明
   previous slot、左板 Status V2 精确证明无 active/completed evidence，才写入有序
   `None` coordinator 记录。不伪造双板 tombstone，不非原子删 A/B key。
5. 接受 V1 `StatusResponse`（仅供发现旧固件），但**删除了 V1 ACK 作为 terminal/release
   证据**的路径——V1 ACK 没有 tx/image 身份，只增加模糊性。

### 3.4 共享代次（`common/OtaGeneration.h`）

右板嵌入身份里 `app_version` 曾硬编码 `"makcm-g15"`，而 `generation` 字段走共享头，
两者不一致导致 **gen16 的右板包根本打不出来**。现在统一为
`MAKCM_OTA_PAIR_APP_VERSION`，两板共用。回归测试禁止固件里出现任何
`"makcm-g<数字>"` 字面量。

**升代只需改 `common/OtaGeneration.h` 一处。**

### 3.5 Host 编排器（`tools/update_proxy_pair.py`）

- 任何写入前先读取并持久化**两板**精确 Status V2；confirmed no-op 也要求两板 tombstone。
- `--controller-detached` 检查移到停 runtime 之前。
- per-port `flock`（`/run/lock/makcm-ota-<hash>.lock`），拒绝同串口并发。
- 参数校验前置（无副作用），拒绝替换 ota tool / Python 解释器。
- 删除 manifest wildcard 和 journal package-identity 迁移（不保留向后兼容）。
- 打包时校验 `validate_intrinsic_identity`。

### 3.6 2026-08-07：阻断 V2 OTA 的四个缺陷

删掉探针后第一次尝试 gen17 双板 OTA，连撞四个独立缺陷。四个都不是"调参"问题，
每个都单独足以让完整 V2 协议**永远跑不通**——这解释了 2.4 为什么一直欠着。

**#1 构建系统：升代不会重建 app descriptor**
（`MAKCM_ESP32s3_Device_Mouse_Left_IDF/CMakeLists.txt`）

`PROJECT_VER` 在 **configure 期**从 `../common/OtaGeneration.h` 解析出来，但 CMake
不知道这个头是 configure 输入。改完代次后 CMake 只重编源码、不重新 configure，
app descriptor 仍写着上一代的 `makcm-g<N-1>`。签名时报
`intrinsic version 'makcm-g16' does not prove generation 17`，**看起来像打包 bug，
实际是构建缓存**。修法是把这个头登记进 `CMAKE_CONFIGURE_DEPENDS`。

> 教训：我最初的本地自检是"在 bin 里 substring 搜 `makcm-g17`"，它假阳性通过了
> （字符串来自别处）。自检必须调用打包器自己的 `validate_intrinsic_identity`，
> 不能另写一份弱化版。

**#2 Host 工具：Status V2 的 JSON 投影是手抄的**
（`tools/makcm_ota_tool.py`）

`_frame_info` 里手工列举要输出的字段，漏了 7 个。编排器
`update_proxy_pair.py` 在**发现阶段**就要求 `STATUS_V2_REQUIRED_FIELDS` 齐全，于是
每次 OTA 第一步就死在 `left Status V2 is missing exact board facts`。
修法是从 dataclass 派生：

```python
for field in dataclass_fields(status):
    value = getattr(status, field.name)
    payload[field.name] = value.hex() if isinstance(value, bytes) else value
```

**手抄的字段列表必然漂移。凡是"板端事实的完整投影"，一律从 schema 派生。**

**#3 右板：`fillStatusV2()` 从不填 `active_generation`**
（`MAKCM_ESP32s3_HID_Mouse_Right/src/ota_receiver.cpp`）

它只在有 active 事务或墓碑时才被间接带出来，Idle 且无墓碑的健康右板报 `0`。
host 于是把右板读成"peer generation 0"，拒绝 rolling-activation 兼容性检查。
**这个症状会诱导人去修快照**，但快照本来就是对的。

`active_generation` 是**当前运行镜像**的属性，不是任何快照记录的属性，必须在所有
状态下无条件上报：

```cpp
status.active_generation = kCurrentGeneration;
```

同时在两侧收紧，让这种包再也无法通过：`common/OtaStatusV2.h` 的
`validStatusV2()` 和 host 的 `decode_status_v2()` 都把 `active_generation == 0`
列为无条件非法。实机验证：`right_preflight` 从 `active_generation=0` 变 `=16`。

**#4 右板：维护态下 `OTA_MAINT_BEGIN` 被自己吃掉**（本轮根因）
（`MAKCM_ESP32s3_HID_Mouse_Right/src/esp_tasks.cpp`）

3.2 说"维护态下仍解析文本 `OTA_MAINT_BEGIN`"，但那个门是**逐字节**测 `'M'` 的。
于是 `OTA_MAINT_BEGIN` 里面那个 `M` 被分流进 MKOT 解码器，文本行拼出来是
`OTA_INT_BEGIN`，匹配不上任何命令，静默丢弃。

后果：一块被中断的 prepare 闩死在维护态的右板，会**静默吞掉唯一能救它的命令**，
表现为"UART 死寂"——正是 4.1/4.4 那种最容易误判成硬件故障的现象。修法是只在
文本行边界上才认 magic：

```cpp
if (otaReceiver.isMaintenanceActive() &&
    (otaReceiver.decoderBuffered() != 0 ||
     (commandLength == 0 && byte == makcm::ota::kMagic[0]))) {
```

四个修复都有回归测试锁死（`tests/test_right_ota_bootstrap.py`、
`tests/test_makcm_ota_tool.py::StatusV2JsonTest`）。

### 3.6.1 #5：左板可以 quiesce 进一个主机侧没有出口的状态

修完前四个后第二次尝试 gen17 OTA，右板在左板 quiesce 期间不再应答，于是撞上一个
**闭合的死锁**——它比前四个更严重，因为受害者是面向 Windows 的左板：

1. `otaQuiesce()` 会 `secondary.setOtaMaintenance(true)`，而 `secondary.cpp:336`
   在维护位打开时**丢弃所有文本行** → CH343 文本面全死（实测 `LEFT_STATUS` 0 字节，
   同一根线上的 MKOT 仍然正常）。
2. `tryReleaseOtaQuiesce()` 要 `ota_unpaired_idle_release_authorized`，而这个标志
   **只能由右板的 Status V2、或左板 Abort/Confirm 的 ACK 点亮**。
3. 右板不应答 ⇒ 第 2 条永远不成立。
4. 唯一还活着的平面是 MKOT，但 Idle 的左板 `transaction_id_ == 0`，
   `ota_update_manager.cpp:516` 把**任何 ABORT 都 NACK 成 `WrongTransaction`**
   （两个事务号都实测过）。

结论：右板一旦在 bootstrap 期间失联，左板只能物理断电才能回来，期间 Windows 透传
一起没。这直接违反 §1.2——一块 Idle、无本地事务、无 pair 记录的左板，它的状态是
**本地事实**，不该由对端事实决定数据面放不放。

修法是把"等对端"这件事限制到它真正有意义的场合：只有当本次会话真的转发过可能让对端
**留下在途镜像**的命令时，才需要对端事实。HELLO/ENTER/STATUS 做不到这一点。

```cpp
const bool bootstrap_only_session =
    !right_image_traffic_forwarded &&
    ota_manager.state() == makcm::ota::State::Idle &&
    (!pair_record_valid ||
     pair_record.stage == makcm::ota::PairStage::None);
```

`right_image_traffic_forwarded` 在 `forwardRightOtaRequest()` 里按命令置位
（Manifest/Data/Prepare/Activate/Confirm/Abort），释放时清零。同时 `otaQuiesce()`
现在自己 arm `right_link_reset_at_ms`——否则右板一次都不应答时，主循环里的释放
重试根本不会被求值。

注意这不是超时兜底：转发过镜像流量的会话**仍然 fail-close**，因为那时对端确实
可能有在途镜像。回归测试
`test_bootstrap_only_quiesce_releases_without_a_peer_fact`。

### 3.6.2 #6（根因）：UART 环形缓冲比一个最大 MKOT 帧还小

这才是"完整 V2 OTA 从来没跑通过"的真正原因。前面所有的"莫名 timeout"都是它。

证据来自右板 NVS 里那份 active 快照（COM6 `read_flash 0x9000 0x5000` 读回来解出来的）：

```
active.present=1  phase=Receiving  last_cmd=0x03(MANIFEST)
next_offset=0     last_seq=2       tx=3394975884   ← gen17d 那次
```

**MANIFEST 收下了，第一个 DATA 块一个字节都没进去。** 之前几轮我一直以为卡在 ENTER，
方向完全错了——ENTER 和 MANIFEST 从来都是好的。

```
kMaxWireSize = kHeaderSize(24) + kMaxChunkSize(4096) + kCrcSize(4) = 4124
右板 Serial1.setRxBufferSize(4096)              ← 比一个帧少 28 字节
左板 SecondaryConfig::rx_buffer_size = 4096     ← 同一个数
```

MANIFEST 上线只有 220 字节，任何时候都过得去；DATA 4124 字节，**环里放不下一个整帧**，
于是能不能收全取决于 RX 任务有没有恰好被调度——掉一个字节 → CRC 失败 → 板子什么都不回
→ 主机只能报 timeout。左板那侧同样中招，只是 gen17d 还没走到左板 prepare 所以没暴露。

两边都改成从协议常量推导，不再出现魔数：

```cpp
Serial1.setRxBufferSize(2 * static_cast<int>(makcm::ota::kMaxWireSize));   // 右板 main.cpp
int rx_buffer_size = 2 * static_cast<int>(makcm::ota::kMaxWireSize);       // 左板 secondary.h
```

回归测试 `test_every_mkot_ring_holds_a_whole_data_frame` 同时锁死三件事：两个环都必须
写成 `makcm::ota::kMaxWireSize` 的表达式、都不许再出现字面量 `4096`、左板 UART1 桥
的环必须 ≥ kMaxWireSize。

> 教训：任何"帧协议 + 环形缓冲"的组合，环必须 ≥ 一个最大帧，且这个关系要由代码
> 表达出来。写成两个独立的字面量，就等着某次改 chunk size 时再炸一遍。

### 3.6.3 右板 coredump：USB Host 栈在 bootstrap 拆机时断言

排查 #6 期间从 coredump 分区（`read_flash 0x350000 0x10000`）读到一份**历史**转储
（app SHA 与当前镜像不符，命令缓冲里是更早的事务号，所以不是本轮崩的）：

```
assert failed: hcd_urb_dequeue hcd_dwc.c:2625
  (urb->hcd_ptr == (void *)pipe && urb->hcd_var == URB_HCD_STATE_DONE)
崩溃任务: usbClientTask
RxTaskSerial1 栈里: "OTA_MAINT_BEGIN 1977355773 16"
```

即 OTA bootstrap 触发 USB 客户端拆除时，IDF 的 host 栈自己断言了。**这是一条真实的
未修缺陷**，只是本轮无法复现（当前流程要求 OTA 前拔掉手柄，`hostState` 是 `Empty`，
走不到那条 `Cleaning` 路径）。有手柄在线时发起 OTA 会走到，需要单独复现和修。

已把 coredump 分区擦掉，所以下一次读到的一定是当前 build 的转储、可以直接出 backtrace。

### 3.6.4 #8：左板没有任何事后取证通道（已修，永久能力）

左板**没有 console**（UART0 是协议，USB 是代理），而 `sdkconfig.defaults` 里
coredump 三个开关是 `TO_FLASH=n / TO_UART=n / TO_NONE=y`。于是一次 panic 在世界上
留下的全部痕迹就是 `LEFT_STATUS` 里的 `reset=4`——**够知道它崩了，永远不够知道它在
哪崩**。原注释的理由是"不要把二进制 coredump 字节注进 CH343 命令帧"，这个理由只约束
**UART** 通道；它顺带写的"reset/stall 计数器提供足够的运行时证据"已被实机证伪。

改为 `TO_FLASH=y`（`TO_UART` 保持 `n`）。coredump 分区 `0x3C0000/0x10000` 在 A/B
schema 里**本来就已经预留**，所以不动 partition_schema、不动 manifest、不需要迁移。
`tests/test_left_idf_control_uart_config.py` 里对应断言同步改成"UART 上什么都不许有，
但必须有 flash 取证通道"——那才是这条约束的真实意图。

注意：ESP-IDF 只在 `sdkconfig` **不存在**时才应用 `sdkconfig.defaults`。改完必须
`Remove-Item sdkconfig` 再 build，否则改动**静默不生效**。

### 3.6.5 #9（真正的根因）：`makcm_bridge_rx` 栈溢出在 `persistPairRecord`

本轮实机现象（旧镜像 `left_bootstrap_gen16.bin`，SHA `F979C70B…`）：

```
LEFT_STATUS  reset=4(PANIC) sec=2/0/0/0/1     <- 左板答得好好的
-> 一个 target=Right 的 MKOT STATUS
LEFT_STATUS  SILENT (t+1s / t+5s / t+12s 都不答)
重开串口（重发 115200->4M 切换）
LEFT_STATUS  reset=4(PANIC) sec=2/0/0/0/1     <- sec 计数器回到 2 = 板子重启了
```

**关键取证技巧**：host 只在打开串口时发一次 115200→4M 切换，所以一块 panic 重启、
以 115200 回来的板子在 4M 口上看起来是"永久静默"。**重开串口能让它复活**——不重开
就会把一次重启误读成对端超时。这一条把之前多个 session 的"右板不应答"全部解释掉了：
根本不是右板，是左板在帧还没上 UART1 之前就崩了（所以 `rx=0`、`rtx` 幸存、pair
记录卡在 stage 1）。

**3.6.4 的 coredump 立刻给出了 backtrace**（`exccause 0x41 DebugException` =
栈末尾 watchpoint）：

```
Crashed task: 'makcm_bridge_rx'
#0 persistPairRecord()                     app_main.cpp:891
#1 mutatePairRecord(stage=RightPrepareIntent) app_main.cpp:988
#2 observeRightOtaResponse()               app_main.cpp:1493
#3 bridgeOtaFrame()                        app_main.cpp:1800
#4 BridgeTransport::handleByte()           bridge_transport.cpp:318
#5 BridgeTransport::rxTask()               bridge_transport.cpp:286
   a14 = 0x15d3f520 = 366212384（就是本事务）
```

**`bridgeOtaFrame` 只有在右板的完整 MKOT 帧到达 UART1 时才会被调用**——也就是说
**右板一直在正常应答**，是左板在解析它的应答时把自己的栈撑爆了。

根因：这条路径同时在栈上放三个 `PairTransactionRecord`（`mutatePairRecord` 的工作
副本、`persistPairRecord` 的**传值形参**、以及它的 `verified` 回读副本，每个 wire
896 B），再叠加 `nvs_set_blob/commit/get_blob` 的内部开销。而
`bridge_config.task_stack` **在 app_main 里从来没设过**，吃的是
`bridge_transport.h` 的默认值 **6144**。实测这条路径要 ~8.1 KB。

Secondary 任务早就因为**同一类问题**（`km.caps()` 撑爆栈，见 app_main.cpp:2287）被
提到 12288；两个任务都会进这条 OTA 路径，却只有一个被按它调大过。

**修法**：`bridge_config.task_stack = 12288`。修完实测 `bstack=4144`（12288 里还剩
4144），确认 6144 差了约 2 KB。

**顺带修掉的观测缺陷**：`BridgeTransport` 当时**没有**栈水位计，`LEFT_STATUS` 里的
`stack=` 只是 Secondary 任务的余量——所以在 `makcm_bridge_rx` 正在溢出的整个过程中，
`stack=7296` 一直纹丝不动、一片健康。已加 `minimumFreeRxTaskStackBytes()`，在
`LEFT_STATUS` 里以 **`bstack=`** 上报。**任何能进 OTA/pair 路径的任务，都必须有自己
的栈水位数字。**

遗留（未做，非阻断）：把那三个 record 从栈上挪走（wire 缓冲区在 app_main.cpp:87-90
早就是文件作用域静态了，record 本身还不是），可以把这条路径的栈需求砍掉约 2.7 KB。
当前靠调大栈已经跑通，按原则 3 先不动能跑的东西。

### 3.6.6 #10：陈旧的 maintenance 闩锁把自己的唯一出口关掉了（死锁）

`startRightMaintenanceBootstrap()` 用 `bridge.sendText("OTA_MAINT_BEGIN <tx> <gen>")`
把右板引导进新事务的 maintenance。而这个函数**恰恰只在
`bridge.otaMaintenance() && tx != right_maintenance_transaction` 时才会被调用**——
也就是"maintenance 正为**另一个**事务持有"的时候。

但 `bridge_transport.cpp:173`：

```cpp
bool BridgeTransport::sendText(const char *text) {
    if (!running() || otaMaintenance() || ...) { tx_errors_.fetch_add(1); return false; }
```

**唯一能把链路重新指向新事务的那条消息，被它要解开的那把闩锁给堵死了。** 后果：
`startRightMaintenanceBootstrap` 返回 false → `sendOtaNack(Busy)` → 而 host 的
`status` 要求 Status V2，V1 的 Busy NACK 被 `_read_response` 静默丢掉 → 主机只看到
`timed out waiting for OTA response`。左板一旦为某个事务进了 maintenance，
**任何新事务都再也 bootstrap 不起来**，只能物理重刷。

还有第二层陷阱：`txTask()` 里 `if (otaMaintenance() || otaModeChangePending()) continue;`
会把已入队的 item **直接丢掉**，所以光把 `sendText` 的门去掉没用，必须绕开队列。

**修法**：新增 `BridgeTransport::sendOtaText()`——不受 maintenance 门限制，且像
`sendOta()` 一样在 `tx_mutex_` 下**同步** `uart_write_bytes`（互斥保证整行不会插进
一个 MKOT 帧中间；右板本来就设计成在 latched maintenance 下仍接受这一行，见
`test_maintenance_serial1_keeps_text_bootstrap_audible`）。
`startRightMaintenanceBootstrap` 改用它。

**`sendText()` 保持原样**：它是 `DEBUG_STATUS`/`READY` 的诊断转发，那个确实是数据面，
maintenance 期间就该关。这是 #7 在 CH343 侧修过的同一类缺陷在 UART1 侧的重演——
**OTA 控制平面和诊断/数据平面必须分开判定，不能共用一个门。**

### 3.6.7 #11：#10 的收方向镜像——maintenance 期间左板听不见 `OTA_READY`

修完 #10 后 `txerr` 归零、`OTA_MAINT_BEGIN` 确实发出去了、`rtx` 也从陈旧值清成 0，
但 bootstrap 仍然完不成：`text=0`、`bstack=11184`（几乎没动，说明右板的 MKOT 帧一次
都没被解出来）。

`bridge_transport.cpp` 的 `handleByte()`：

```cpp
if (otaRxArmed()) {
    ... 每一个字节都喂给 ota_decoder_ ...
    return;              // <- 永远走不到下面的文本路径
}
```

**maintenance 期间左板把 UART1 上的每个字节都当 MKOT 吃掉，文本一个字节都不解析。**
而 `handleOtaBridgeTextLine()`（`OTA_READY` / `OTA_PENDING` / `OTA_UNAVAILABLE` 的
**唯一**消费者）只能从文本路径进入。于是 bootstrap 的**问**发得出去、**答**收不回来。

**修法**：照搬 CH343 侧已经验证过的做法（`Secondary::handleByte`）——只在
**文本行边界上遇到精确的 MKOT magic** 时才把流交给 MKOT 解码器，其余字节仍走文本
路径。新增 `ota_line_idle_` 跟踪行边界，并在 `applyPendingOtaMode()` 里复位它
（模式切换本身就是一个帧边界，否则切换时正好在半行中间会导致解码器再也 arm 不了）。

二进制数据面依然是关的：bridge 帧走 `decoder_`，这条路径根本够不到它。maintenance
期间可听见的只有 MKOT 和 OTA 控制文本。

**#10 + #11 是同一条教训的两半**：OTA 控制平面（`OTA_MAINT_BEGIN` / `OTA_READY`）
和诊断/数据平面共用了一个 `otaMaintenance()` 门。**收发两个方向都要分开判定**——
只修一半，问发得出去、答收不回来，现象和完全没修一模一样。

### 3.7 已知但本轮未修（有意留下）

`snapshotReady_ == false` 时 `fillStatusV2()` 直接放弃，`sendStatus()` 退回 V1
NACK，而 `update_proxy_pair.py` 拒绝 V1 → **快照损坏的板无法纯靠 host 驱动恢复**。

根因是 Status V2 线格式**表达不了"我的快照不可用"**：`validStatusV2()` 要求
`kEvidenceSnapshotValid`。要修得动线格式（加一个显式的 snapshot-unusable 证据位），
超出本轮范围，且会和正在验证的 gen17 包纠缠。**下一代改。**

### 3.6.8 #12（真正的 GO-1 根因）：一次失败的尝试永久占用了它的 generation

`validSnapshot()` 里的单调性闸门对**任何** completed 墓碑都成立：

```cpp
active.identity.generation <= completed.identity.generation  → 拒绝
```

左板的墓碑记着 `outcome=2 (Aborted)`、`terminal_command=9 (Abort)`、
image_sha256 是 **gen17** 的镜像。于是第一次 gen17 尝试失败并被 abort 之后，
**任何** gen17 事务再也无法编码——`encodeSnapshot()` 在碰 NVS 之前就返回 false，
主机看到的是 `FlashWriteFailed`。这就是 V2 双板 OTA 从来没成功过一次的原因，
而且它伪装成 flash 故障，把三个 session 的排查引向了完全错误的方向。

修复：只有 `Outcome::Confirmed` 的墓碑才占用 generation。Aborted/RolledBack
说明镜像根本没装上、板子还跑在旧 generation 上，重试同一个 generation 是前进
而不是重放。事务 id 不等的约束保持无条件。

**实机验证**：修复后左板单板首次跑通 `ENTER → MANIFEST → DATA(483072 B 全写完)
→ Prepared`（`state=3 phase=2 next_off=483072`）。这是 V2 事务有史以来走得最远的一次。

### 3.6.9 #13：`persistCommandFact` 把板级标志写进了事务字段

`validSnapshot()` 要求 `active.local_validated` 恰好等于
`phase ∈ {LocalValidated, PairPending}`——它描述**本次事务装的镜像**是否已验证。
两块板的 `persistCommandFact` 都写的是板级的 `local_boot_validated_ /
localBootValidated_`，而健康板子这个值恒为 true。已改为从 phase 推导。

（注：单独修这一个并不足以跑通，#12 才是当时的阻塞点；但两者都是真缺陷。）

### 3.6.10 #14：右板 `hostState` 可以孤儿式卡在 `OtaReserved`

`serviceOtaBootstrap()` 的释放逻辑以 `otaHostReserved` 为条件，但把 hostState
留在 `OtaReserved` 的那两条路径（预约 CAS 失败、bootstrap 等待超时）**恰好都已经
把 `otaHostReserved` 清成 false 了**，所以修复永远不触发。后果：此后每一次
`OTA_MAINT_BEGIN` 都立即回 `OTA_UNAVAILABLE`，每一次手柄热插拔也永远排队等一个
没人持有的预约，只有断电能解。已改为由 hostState 驱动释放。

### 3.6.11 #15：左板 Fault + 无持久记录 = 没有出口

`ENTER` 在 Fault 下被拒为 `InvalidState`；其余命令被事务闸门拒为
`WrongTransaction`——包括本该用来清 Fault 的 `ABORT`。唯一出路是拔线擦 NVS。
已加恢复入口：`Fault && !snapshot_.active.present` 时 ABORT 可以自带新事务 id。
判定刻意基于**持久记录**而非易失的 `transaction_id_`：ENTER 之后、manifest 之前
进 Fault 的板子持有一个从未写进 Status V2 的 id，主机根本叫不出来。

### 3.6.12 观测能力：`scf=` 把 7 个失败点分开

每条 durable 命令都走 `commitPendingSnapshotV2()`，它的 7 种失败全部以同一个
`NackReason::FlashWriteFailed` 上报，其中 4 种在任何 `esp_err_t` 存在之前就返回，
`last_nvs_error` 只是上一次无害读取的残留。现在 `LEFT_OTA_DIAG` 输出
`nvs=<esp_err> ota=<esp_err> scf=<site>`：

| scf | 含义 |
| --- | --- |
| 0 | 无失败 |
| 1 | 没有 NVS handle |
| 2 | role 不匹配（`loadSnapshotV2()` 失败后的永久后果） |
| 3 | commit_sequence 饱和 |
| 4 | **encodeSnapshot 拒绝**（`validSnapshot` 不过） |
| 5 | `nvs_set_blob` 失败 |
| 6 | `nvs_commit` 失败 |
| 7 | 回读不一致 |

`scf=4 + nvs=0` 正是抓到 #12 和 #13 的那条证据。

### 3.6.13 #16：pair 记录的事务身份靠推断，陈旧的对端事实会劫持它

`mutatePairRecord()` 原来从 `right present ? right : left` 推断"这次改动属于哪个
事务"。但调用方普遍会把记录里自己的对端事实原样传回去，于是**陈旧的 right 永远赢
得这个优先级**，把记录的身份据为己有；真正新鲜的 left 事实随后被拿去和它比对，报
`BoardFactMismatch`。更糟的是切换守卫也读同一个推断，所以新事务从来没有被识别成一次
切换，守卫根本没运行过。

修法：事务号改成 `mutatePairRecord(transaction_id, ...)` 的显式参数——调用方永远知道
它（来自请求帧，或来自产生该事实的那次 status 读取）。不属于该事务的事实直接**丢弃**
而不是报错（那只是正常的陈旧携带），`BoardFactMismatch` 由此收窄回它本来的含义：同一
事务内部的 generation 不一致。`updatePairFromLocalStatus` 和 `observeRightOtaResponse`
里决定 stage 的逻辑也要读同样过滤后的指针，否则一条陈旧事实能凭空造出
`BothLocalValidated` 或 `RolledBack`。

### 3.6.14 #17：单边 rollback 是第三种终态，但没有出口

记录停在 `RollbackIntent` 时，只有"精确 pair commit"或"精确 pair rollback"能让新事务
displace 它。而 `RolledBack` 要求**两块板**的事实都在且都已回滚——对端从未加入的事务
永远到不了。唯一的退役动作 `retireOneSidedRightRollback()` 只作为
`observeRightOtaResponse()` 的副作用触发，即只在右板 STATUS 正在被折叠进记录的那个瞬间；
主机在那个窗口里挂掉、或右板再不发 STATUS，记录就永久卡死。与 #14 同型：**修复被挂在
一个失败本身倾向于关闭的窗口上**。

新增 `pairRecordHasExactOneSidedRollback()` 作为第三种终态，并把可退役性放到切换守卫里
判断——于是每一个新事务都是一次清理机会，而不是一个偶然窗口。

### 3.6.15 #18：`running_image_sha256` 是实测值，`previous_image_sha256` 是记录值

`exactBoardRollbackFact()` 要求二者相等。但前者每次都由
`esp_partition_get_sha256(running)` 现场测量，后者是事务当初写下的常量。**任何越过 OTA
的写入——开发机烧录、产线烧录、救援烧录——都会永久拆散它们**，于是记录再也无法退役。
硬件上就是这么卡住的：`LEFT_PAIR ... slot=17/16/16/16 imgeq=0`，槽位证明板子确实在
previous 槽上，只有哈希对不上。

判定应当承载的安全性质是"这个事务的镜像永远不会启动"，而不是"板子还跑着该事务记得的
那些字节"。所以 `pairRecordHasExactOneSidedRollback()` 不再调用
`exactBoardRollbackFact()`，改为要求 `boot_slot != target_slot && boot_slot ==
previous_slot`。`exactBoardRollbackFact()` 本身保持强判定，仍供
`retireOneSidedRightRollback()` 使用。

### 3.6.16 #19：没有任何板子参与过的 ABORT 被误拒

恢复用的 ABORT 会**故意**编一个新事务号（板子正是因为没有可用事务号才需要恢复）。
`preparePairIntentForRequest()` 拿它去写 rollback intent，在 #16 修好之后所有事实都被
正确丢弃，于是没有 generation 可以播种，报 `SeedFactIncomplete` → 主机收到
`CompatibilityRejected`，恰好打死唯一一条能解开死板的命令。

正解是两者都不做：没有任何板子加入过这个事务，就没有 pair intent 要写。记录原样不动
（它仍然可被后续事务 displace），ABORT 照常执行。

### 3.6.17 观测能力：`pf=`、`prec=` 与 `LEFT_PAIR`

pair 记录是与 OTA 快照并列的第二个持久存储，报同一个 `FlashWriteFailed`。现在：

- `pf=<site>/<esp_err>`：21 个失败点。1–14 是存储类，15–21 根本不是存储问题
  （15 `TransactionSwitchBlocked`、16 `SeedFactIncomplete`、17 `BoardFactMismatch`、
  18 `LocalStatusUnusable`、19 `ResponseCorrelationMismatch`、20 `PeerStatusUnusable`、
  21 `PeerResponseUnusable`）。多数站点没有真正的 `esp_err_t`，那个数字只是占位。
- `prec=<tx>/<l.present>:<l.phase>/<r.present>:<r.phase>`：记录自身的事务与参与者。
  注意 `rtx=` 是**右板维护事务**，是另一个数字，长得很像但不是这个。参与者决定了停滞
  的是双边事务还是单边事务——两者出口不同。
- `LEFT_PAIR` 文本命令：直接打印三个可退役判定的结果，以及它们读的每一个字段。
  `imgeq=0` 就是这么抓到 #18 的。

### 3.6.18 #20：右板 manifest 拒绝把 5 种原因压成 1 个码

`toNack()` 把 `BadManifest / BadRole / BadHardware / BadPartition / BadImage` 全部映射成
`ManifestRejected`。其中 `BadHardware` 覆盖了 `peerGeneration_ == 0`——**只有板子知道的
状态**——却和"包本身有问题"（主机自己就能复核的字节）长得一模一样。

`NackReason` 里本来就有准确的名字：`BadRole → WrongTarget`，
`BadHardware → CompatibilityRejected`。剩下三种描述的是 manifest 自己的字节，主机手里就
有，保持 `ManifestRejected` 即可。不需要改动线格式。

---

## 4. 被实机推翻的错误结论（避免重蹈）

### 4.1 `PROBE_RIGHT_HELLO` 语义是反的（探针已删除）

这是当时加的诊断命令，**语义与直觉相反**：

- `ok=1` = 右板**锁在维护态**（异常）
- `timeout` = 右板**健康 Idle**（正常）

因为正常 Idle 时二进制 MKOT 字节进的是桥接层 COBS 解码器，被当噪声丢弃；只有已进
维护态才会喂给 OTA 接收器。我连着两轮把 timeout 读成"右板死了"，由此得出
"UART1 物理链路坏了"的错误结论。

**探针已于 2026-08-07 删除**（`runProbeRightHello`、`BridgeTransport::sendOtaRaw`、
`setDiagnosticOtaListen` 及其 `diagnostic_ota_listen_` 状态位）。它唯一能区分的两种
状态里，健康和死亡都表现为 timeout，价值接近零而误导性已被证明。`DEBUG_STATUS`
是唯一的诊断入口。

顺带关掉的一个后门：`sendOtaRaw` 绕过独占维护门写 UART1，`setDiagnosticOtaListen`
让 UART1 在非维护态被当 MKOT 解码。现在 `otaRxArmed()` 只看 `ota_maintenance_`，
**UART1 的 MKOT 解码只有一个门**。回归测试
`test_production_left_has_no_diagnostic_uart1_probe` 锁死这一点。

### 4.2 探测脚本的首命令必被吞

A/B/C 对照实验结果：

| 做法 | 第一条命令 | 第二条 |
|---|---|---|
| 板子已在 4M，仍发 115200 波特率切换序列 | **0 字节** | 486 |
| 不发切换序列，直连 4M | 486 | 486 |
| 发切换序列后补一个 `\n`，再发命令 | **486** | — |

原因：板子已在 4M 时用 115200 发那 9 字节，板子收到位错乱的垃圾，进了左板行缓冲，
于是下一条命令变成 `<垃圾>LEFT_STATUS`，匹配不上被静默丢弃。

**生产工具一直是对的**——`SerialTransport.__init__` 在 4M 重连后会写
`SERIAL_FRAME_BOUNDARY = b"\n"`。只有临时探测脚本漏了这步。
早期"Enter 成功过一次、之后再也复现不了"就是这个原因，不是玄学。

**任何新的诊断脚本必须在重连后先发一个 `\n`。**

### 4.3 空 otadata 不会导致启动失败

我曾判断"空 otadata 的 app 没 mark-valid，断电后会被标 ABORTED 然后起不来"。
核对 `bootloader_utility.c` 后确认**这是错的**：两份 otadata 全 `0xFF` 且无 factory
分区时，bootloader 走 `ota_has_initial_contents` 分支，直接启动 ota_0，
**不进入 `NEW → PENDING_VERIFY` 状态机**，所以不会自我降解成 ABORTED。

右板 partition 表只有 app0/app1、没有 factory，正好命中这条分支。

### 4.4 硬件从头到尾没有问题

曾一度判断"UART1 至少右→左方向不通，需要逻辑分析仪"。最终证明链路完好，
所有"死寂"都由 4.1 和 4.2 两个观测缺陷造成。

### 4.5 "右板不应答转发的 MKOT 帧"——错的，是左板在复位

这是本轮最贵的一个错误结论，跨了多个 session。真相见 3.6.5：**右板从头到尾都在正常
应答**，是左板在解析这份应答时（`bridgeOtaFrame → … → persistPairRecord`）把
`makcm_bridge_rx` 的栈撑爆、复位了，于是应答永远走不回主机。左板以 115200 重启回来，
而 host 只在开串口时发一次波特率切换，所以 4M 口上看起来像对端永久静默。

**特别注意 `LEFT_STATUS` 的 `rx=` 不能用来判断"右板有没有说话"**：`rx=` 是
`bridge.rxFrames()`，只数**bridge 协议帧**，**不数 MKOT 帧**。整个排查过程里
`rx=0 text=0 rxerr=0` 一直成立，而右板其实每次都回了 400 字节的 Status V2。
要判断右板是否应答，只能直接听 MKOT 帧，或者看 `bstack=`/coredump。

**排查顺序的教训**：先证明**近端**没事，再谈对端。一个 target=Left 的 MKOT STATUS
（左板直接答 400 字节 V2）只要一次就能把 CH343 链路、4M 切换、帧边界、解码器 arming
全部排除掉；我却先去猜右板。同理，`tools/makcm_ota_tool.py` 的 `_read_response` 里有
**九个静默 `continue`**（flag 不匹配、payload 解不动、`last_command` 对不上……），
全部塌缩成同一句 `timed out waiting for OTA response`——**排查时不要用它读线**，
直接 `FrameDecoder` 打印收到的每一帧。

---

## 5. 接下来的工作

### 5.0 当前阻塞点（2026-08-07 末）

**左板路径已通。** pair 记录的失败点归因（#16–#19，见 3.6.13–3.6.17）落地后，
硬件上第一次拿到：

```
abort(tx=3409453827) ok          <- Fault -> Idle，恢复入口可用
OK   ENTER     0.05s
OK   MANIFEST  1.31s
OK   DATA      24.94s
OK   PREPARE   0.30s             <- V2 PREPARE 第一次成功
end: state=3(Prepared) phase=2 next_off=483072
```

卡死的记录（tx=1508245409 / stage=12）被新事务 displace 掉了，
现在是 `prec=2994686691/1:2/0:0`、`corrupt=0`、`pf=0`。

**两块板都已刷入本轮修复**：左板 #12/#13/#15/#16–#19，
右板 #13/#14（COM6，写 otadata + app0，保留 NVS）。

**新的阻塞点在右板。** `pair-prepare` 现在能走到右板并被它拒绝：

```
PAIR_PREPARE_EXIT=2
STDERR: makcm_ota_tool: OTA NACK reason=8 (manifest_rejected)
```

右板先于左板执行，所以左板的 `state=3` 只是上一次单板 probe 的残留。
`ManifestRejected` 原本压着 5 种原因（#20，见 3.6.18），已拆成三码；
**下一步是刷入这个拆分后的右板固件，重跑 `pair-prepare`，用新的 reason 定位**：

- `WrongTarget` → manifest 的 role/image_kind 不对；
- `CompatibilityRejected` → 硬件版本/工程名/代次单调性，或 `peerGeneration_ == 0`
  （右板刚断电重刷，很可能还没学到左板代次——这是当前最大嫌疑）；
- 仍是 `ManifestRejected` → manifest 自己的字节，主机手里就有，可离线复核。

按优先级：

1. ~~**删除 `PROBE_RIGHT_HELLO`**~~ — **源码已完成（2026-08-07）**，左板构建 PASS、
   224 + 29 + C++ host 测试全绿。**剩余：把这个干净镜像真正刷进左板**
   （COM3 直刷，或让它随 gen17 OTA 一起下去，见第 3 条）。
2. **重新签名右板包**。`diagnostics/ota/go1_freeze_20260806/right-gen16.mkota` 哈希的是
   **修复前**的镜像，不能用于协议 OTA。当前右板运行的是 `da1c483c…979007`。
3. **双板 gen17 签名 OTA**。这是第一次真正跑完整 V2 端到端，也是关掉 GO-1 的唯一证据。
   两板现在都在 gen16，代次已对齐，gen17 包的 `min_peer_generation` 可取 16。
4. gen17 完成后**断电复查**：两板仍为 gen17、Idle、`local_boot_validated`，
   且手柄透传正常。这一步才验证 `restoreFromSnapshotV2()` 的 Confirmed 墓碑分支。
5. 剩余 GO 门禁：shared C++/Python golden vectors（已有基础）、USB descriptor golden、
   production/probe 构建分离、断电故障注入矩阵。

### 5.1 实机操作要点

**三个 USB 口的角色（不要搞混）：**

| 口 | 接什么 | 能做什么 | 禁止 |
|---|---|---|---|
| USB1 | Windows | 纯透传数据面 | **非 BOOT 模式下不可读**。用 esptool 去"探测"它会把左板**复位**，直接打断正在进行的会话/OTA |
| USB2 | Linux 副机（CH343 → 左板 UART0） | 唯一可信更新入口；`ssh` 可达 | — |
| USB3 | 手柄 / 鼠标（右板 USB Host） | 被更新期间 detach 的对象 | — |

USB1 只有在按住 BOOT 上电进 ROM 下载模式后才是串口（即 COM3）。**任何时候都不要
对正常运行中的 USB1 跑 esptool / 端口枚举探测**——这是一次自诱发复位，且会被误读成
"左板崩了"。

- 左板下载口是 COM3，右板是 COM6（都需按住 BOOT 上电）。**不要靠 COM 号认板**——
  实测按错板子时照样只冒出 COM3。可靠判据是读分区表：
  `read_flash 0x8000 0x1000` 后跑 `gen_esp32part.py`，左板是 `ota_0@0x20000`
  + `coredump@0x3c0000`，右板是 `app0@0x10000` + `coredump@0x350000`。
- esptool 的 `--after no_reset` 会把板子**留在 bootloader 里**，此后 CH343 上一片
  死寂、看起来像刷坏了。写完固件要么用 `--after hard_reset`，要么让用户断电重上。
  反过来 `--after hard_reset` 会让 COM3 消失，所以"刷完再擦 coredump"这种连续
  操作要么合并进同一条 esptool 命令，要么分两次 BOOT。
- **一块 panic 重启的板子会以 115200 回来，而 host 只在开串口时发一次 4M 切换**，
  所以它在 4M 口上看起来是永久静默。排查时**重开一次串口**再判断死活；不重开就会
  把一次重启误读成对端超时。
- CH343 软复位不可用（DTR/RTS 复位不了），**必须物理断电**。
- 左板单板 OTA 走完 Activate + mark-valid 会停在 `PendingConfirm`，而
  `maintenanceExclusive()` 对 PendingConfirm 返回 true，**USB 透传是关着的**。
  回到 Idle 必须 Confirm，而 Confirm 需要 `otaPairHealth()` → 需要右板。
  所以**左板不要单飞**，否则只能再 COM3 刷一次出来。
- 右板可以单飞：它的 `healthPassed()` 查的是自己的 USB Host 状态，不依赖左板。
- 签名私钥只在可信 Linux（`192.168.137.2`）的 `~/.config/makcm-ota/signing/`，
  不进仓库。**签名走 `tools/sign_pair_release.py`**（从 `common/OtaGeneration.h`
  读代次、本地跑 `validate_intrinsic_identity`、远端签名+校验、产物拉回
  `diagnostics/ota/release_gen<N>/`）。它替代了每代一份的
  `diagnostics/ota/go1_resign_*_gen16.py` 一次性脚本。

**2026-08-07 新增的实机约束：**

- **esptool 无法让板子退出下载模式。** `--after hard_reset` 在这两块板上不起作用。
  刷完必须**先拔 USB3，再物理断电**；顺序反了它会重新进下载模式，然后被误判成
  "固件坏了"。
- **journal 在事务创建时就把包字节 stage 下来，之后一直复用那份拷贝。** 重新签名
  之后如果继续用旧 journal，会**静默刷下去旧镜像**。改包必须换新事务
  （`diagnostics/ota/run_gen17_pair_ota.py` 里的 `RUN` 常量）。
- **被中断的 prepare 会把右板 fail-close 闩在维护态。** 3.6#4 修好之前它连
  `OTA_MAINT_BEGIN` 都吞，只能 COM6 重刷 + 擦 NVS（`erase_region 0x9000 0x5000`）
  才能救回来。
- 只读探活用 `DEBUG_STATUS` / `READY` 走 CH343 文本面即可，右板健康时回
  `RIGHT:ESPLOG_STATUS … registered=1` 和 `RIGHT:USB_ISNULL`。
  **不要用 MKOT 超时判断右板生死**（见 4.1）。
- **维护态下右板对普通命令静默是设计如此**：`esp_tasks.cpp` 的 `serial1Write()`
  在 `isMaintenanceActive()` 时丢弃一切普通回复，只有 `otaReceiver.sendText()`
  绕过这个门。所以"`DEBUG_STATUS` 不回"= 维护态，不等于板子死了。
- 右板没有可用的运行时控制台：`USB_PHY_SEL` eFuse 已烧成 USB-OTG，USB-Serial/JTAG
  在运行态是关的——这就是"不按 BOOT 就没有 COM6"的原因。**唯一的证据来源是回读
  flash**。
- 左板 `LEFT_STATUS` 的 `text=` 是 UART1 收到的文本字节累计数。围绕一次操作前后各采
  一次，就能把"右板没回"和"左板把回复吃了"区分开——这是最便宜的判据。

**回读 flash 的取证清单（右板 COM6，左板 COM3，都需按 BOOT 上电）：**

| 内容 | 右板偏移 | 用途 |
|---|---|---|
| NVS | `0x9000` `0x5000` | Snapshot V2（`OTS2` 魔数）。`phase`/`last_cmd`/`next_offset` 直接说明卡在哪一步 |
| otadata | `0xE000` `0x2000` | `seq`/`state`，确认在跑哪个槽——排除"推理建立在错误镜像上" |
| coredump | `0x350000` `0x10000` | `espcoredump.py --chip esp32s3 info_corefile --core-format raw <elf>` |

`erase_region 0x9000 0x5000` 正好只擦右板 NVS，不碰 otadata。
coredump **不会**被 NVS 擦除清掉，取证前先擦一次，否则读到的可能是几个会话之前的旧
转储（本轮就踩过：app SHA 对不上才发现）。espcoredump 会强校验 app SHA，ELF 必须是
和板上镜像同一次构建的产物。

---

## 6. 关键文件索引

| 用途 | 路径 |
|---|---|
| 共享代次（升代唯一入口） | `common/OtaGeneration.h` |
| OTA 线协议 / 帧 | `common/OtaProtocol.h` |
| 本板 Snapshot V2 | `common/OtaSnapshotV2.h` |
| Pair 事务 schema | `common/PairOtaTransaction.h` |
| 左板 OTA 状态机 | `MAKCM_ESP32s3_Device_Mouse_Left_IDF/components/makcm_core/ota_update_manager.cpp` |
| 左板 pair 协调 / CH343 | `MAKCM_ESP32s3_Device_Mouse_Left_IDF/main/app_main.cpp` |
| 左板 CH343 文本/MKOT 复用 | `MAKCM_ESP32s3_Device_Mouse_Left_IDF/components/makcm_core/secondary.cpp` |
| 右板 OTA 接收器 | `MAKCM_ESP32s3_HID_Mouse_Right/src/ota_receiver.cpp` |
| 右板 bootstrap / USB Host | `MAKCM_ESP32s3_HID_Mouse_Right/src/commands.cpp` |
| 签名/协议 CLI | `tools/makcm_ota_tool.py` |
| Linux 编排器 | `tools/update_proxy_pair.py` |
| 冻结产物与实机证据 | `diagnostics/ota/go1_freeze_20260806/` |

### 6.1 验证命令

```powershell
python tests\run_host_tests.py
python -m unittest discover -s tests -p "test_*.py"
python -m unittest discover -s MAKCM_ESP32s3_HID_Mouse_Right\tests -p "test_*.py"
& '.tools\platformio-venv\Scripts\pio.exe' run -d 'MAKCM_ESP32s3_HID_Mouse_Right'
```

左板构建需先 `. .tools\esp-idf-v5.5.5\export.ps1`，再
`idf.py -C MAKCM_ESP32s3_Device_Mouse_Left_IDF build`。

---

## 7. 工程原则（用户明确要求，请遵守）

1. 不保留向后兼容。过时的直接删，别加兼容层、别写 migration、别留 fallback。
2. 选能满足当前需求的最简单实现。不要预防性抽象，不要多此一举的配置层。
3. 先跑通最小端到端，再往上加。绝不为了未完成的复杂度拆掉能跑的东西。
4. 组件保持模块化，关注点分离。
5. 优先用成熟的、有人维护的库。
6. 先翻项目已有依赖能做什么，再考虑加新包或自己写。
7. 架构决策往长了做，不接受"先这样以后再换"。
8. 先看成熟产品怎么解决同一问题，用已验证的模式。

本轮有两次教训直接对应第 1、2 条：`begin()` 的死代码和右板 `app_version` 漂移，
都源于"在错误逻辑前面加特例"而不是"删掉错误逻辑"。
