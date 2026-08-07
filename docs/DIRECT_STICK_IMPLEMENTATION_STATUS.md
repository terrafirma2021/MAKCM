# Direct-stick 转换器实现状态（DS-04 Q15 0.20 实机门禁）

日期：2026-08-04  
设计规格：`docs/DIRECT_STICK_CONVERTER_IMPLEMENTATION_PLAN.md`  
USB 代理事实基线：`docs/MAKCU_USBPROXY_FS_IMPLEMENTATION_STATUS.md`

本文只记录 direct-stick 新增部分。通用 USB 代理当前事实仍由
`MAKCU_USBPROXY_FS_IMPLEMENTATION_STATUS.md` 定义；本文不改变其中尚未完成的
bus reset、睡眠/唤醒和四路 IN 同窗结论。

## 0. 文档与任务使用规则

从现在起，本文是唯一日常任务入口；不再从多份文档各自挑“下一步”。其他文档仅按
下表在需要时查阅：

| 文档 | 唯一职责 | 什么时候看 |
|---|---|---|
| `DIRECT_STICK_IMPLEMENTATION_STATUS.md` | 当前完成事实、唯一执行队列、阻塞和下一步 | 每次开始/结束任务 |
| `DIRECT_STICK_CONVERTER_IMPLEMENTATION_PLAN.md` | 架构、协议、边界和阶段验收规范 | 实现某项或判定 PASS 时查对应章节 |
| `MAKCU_USBPROXY_FS_IMPLEMENTATION_STATUS.md` | 通用 USB Proxy 的权威事实基线 | 修改 endpoint/FIFO/reset/hotplug 时 |
| `STICK_PROFILE_CAPTURE_WORKFLOW.md` | adapter 工程取证的操作步骤 | 只有进入 DS-02 人工采集时 |

若出现表述冲突：通用 USB 当前事实以 `MAKCU_USBPROXY_FS_IMPLEMENTATION_STATUS.md`
为准；direct-stick 目标设计以 implementation plan 为准；“现在做到哪里、接下来做什么”
只以本文为准。

### 0.1 唯一执行队列

同一时间只把一个未完成项标为 `ACTIVE`。后续项可以做设计预研，但不得越过其依赖
门禁标成 PASS。

| ID | 状态 | 任务 | 前置条件 | 退出门禁 | 你的配合 |
|---|---|---|---|---|---|
| `DS-01` | `COMPLETE` | 实现 guided adapter analyzer：neutral、右/上锚点、顺/逆圆周及 host tests | 无 | 工具能拒绝错误 trace，并生成可审计候选 adapter；不再要求单轴扫轴 | 无 |
| `DS-01B` | `COMPLETE_CAPTURE_CONDITIONAL` | USBPcap discovery、12-marker guided capture、descriptor snapshot 解包、只读 preflight 与审计工具 | DS-01 | submit/completion 去重、abort 保留原始证据、composite target-interface/no-HID 边界、依赖分组门禁及 offline tests 通过；live capture 已有证据，但 live adapter mutation 仍未实测 | 无；不要求用户重复采集 |
| `DS-02` | `COMPLETE_SINGLE_DEVICE_CONDITIONAL` | 对一个代表性 Xbox 风格 identity 做一次 adapter qualification | DS-01B + Wireshark/USBPcap + 实体设备 | 首台 descriptor/report identity、offset/encoding/sign/range/integrity 证据通过；只授权 phase-3 0.20 | 已完成；不要求再重复扫轴或逐设备中心校准 |
| `DS-02R` | `DEFERRED_NO_SECOND_DEVICE` | 验证第二台 exact-same identity 无需重配即可复用 | DS-02 + 第二台 `4131:3519` | descriptor/profile exact hash 一致，原 profile 无修改复用；否则作为新 identity | 当前没有第二台；这是产品复用声明门禁，不阻塞首台受限开发 |
| `DS-02D` | `WAITING_FOR_DEVICE` | 对 DualSense Edge identity 做独立 adapter qualification | DS-01B + DSE 实体设备 | DSE 自身 descriptor/report/layout/integrity 证据通过 | 有 DSE 时再连接；不能借用 Xbox profile |
| `DS-03` | `CURRENT_GEN4_READONLY_PASS_ARM_PENDING` | 使用本地受审条目更新左板并完成 caps/begin/status 握手 | DS-02 + reviewed local entry | 连续 20 轮 caps/status 不重启且 session 稳定；control stack 最少剩余 4096 bytes；exact hash 匹配；unknown profile 零修改；固件 hash 留档；无 qualifying report 时 barrier 必须板端 fail-close | generation 4 已经生产签名 OTA 部署；20 轮只读 caps/status、7200-byte stack 与通用数据面已 PASS。当前 generation 4 尚未重做空载 begin/leave barrier，因此 DS-03 不标 COMPLETE |
| `DS-04` | `ACTIVE_Q15_0_20_LIVE_GATE` | 阶段 3 实机：Q15 上限 0.20，再门禁提升到 0.35 | DS-03 | 字段/方向/回中/cadence/TTL/kill/hotplug/physical override 全部 PASS | 只观察或按提示拔线/轻动实体摇杆，不做游戏曲线操作 |
| `DS-05` | `WAITING` | Host `calibration-wide`、DXGI present timestamp、SEI v2 和 Host↔Secondary↔board 三时钟同步 | DS-04 | 全画面 geometry manifest 正确；时钟 uncertainty 达标；生产 ROI 基线不变 | 启动一次游戏画面用于流和帧率验收 |
| `DS-06` | `WAITING` | standalone synthetic plant calibrator、controller ADS actuator、HIP/ADS 静态完整曲线 | DS-05 | 自动 press/verify/release；wide observer 通过；V1.0 `STATIC_MONOTONIC` profile 交叉验证达标 | 进入指定训练场并保持不操作；不需手动切 HIP/ADS |
| `DS-07` | `WAITING` | `AngularIntent -> stick_rate -> inverse plant` shadow，再受控启用 | DS-06 | requested/accepted/emitted/measured 同轴对齐；mouse live baseline 不变 | 训练场观察/A-B |
| `DS-08` | `WAITING` | legacy `km.move` compat estimator、物理合并 A/B 和 2 小时稳定性 | DS-07 | 孤立 `move(1)` 不脉冲；持续 move 正确转换；全部阶段 6 门禁 PASS | 最终人工体验与异常操作 |

通用 Proxy 的 bus reset、睡眠/唤醒和四路 IN 同窗保持为独立 `P-USB` 工作流；它们不应
与 DS-01/DS-02 混在同一个修改任务中，但在宣告整个产品完成前仍必须按权威状态文档
关闭。`DS-01/DS-01B` 的软件/采集门禁和 `DS-02` 首台条件 qualification 已完成；
`DS-04` 是当前唯一 `ACTIVE` 项。`DS-02R` 因没有第二台设备延期，`DS-02D` 因没有
DSE 等待设备；二者不授权夸大复用范围，但也不阻塞首台 0.20 限幅验证。

### 0.2 固件更新唯一入口

从 generation 4 起，普通 application 更新统一走可信 Linux 副机的 USB2/CH343 签名
双板 OTA；不得再把 Windows USB1、ROM 下载口或 `flash_generic_proxy_pair.ps1` 当成日常
更新入口：

- 唯一自动编排入口为 `tools/update_proxy_pair.py`，底层继续调用
  `tools/makcm_ota_tool.py` 的 `pair-prepare -> pair-activate -> pair-confirm`；顺序固定
  right→left，且明确不是跨两颗 Flash 的原子提交；
- updater 只接受已经签名的 `.mkota` application package 和生产**公钥**，不接受生产
  私钥，不调用 esptool，也不写 bootloader/partition table/otadata；这些基础段只保留为
  人工救砖/分区迁移路径；公钥文件还必须匹配显式固定的 `--public-key-sha256`，当前生产
  PEM hash 为 `EE2378C2EAFAD90859D63D260CBC373C8F125D8EFCFA6427DC8A53536395D399`；
- 必须显式给出 `/dev/ttyACM0`，并由 sysfs 复核 CH343 `VID_1A86/PID_55D3`、serial 和
  bus path；journal 续跑及每个 prepare/activate/confirm/status 阶段前都会重验同一 identity。
  串口被非受管进程占用时 fail-close，不自动 `kill -9`；
- 自动流程仍要求三个独立授权 `--allow-ota --allow-activate --allow-confirm`。确实需要
  prepare 时还要求 `--controller-detached`；如果板上已经是目标 generation/Idle，则执行
  签名、身份和状态核验后零写返回，不要求重复拔手柄；
- journal 以 transaction/package SHA/generation 绑定并原子持久化；中断后只续跑同一
  release。未确认 journal 禁止被新包覆盖；已确认旧 generation 自动归档后才能创建新
  transaction；未完成事务从私有暂存包恢复，不依赖原始发布目录。若板端已经进入
  PendingConfirm 而主机 journal 仍停在 prepared，会先按板端持久状态推进 journal 再 confirm；
- 验签后的左右包先复制到权限为 0700 的事务暂存目录，prepare 前再次核对 SHA-256，
  避免源路径在验签后被替换；新包还必须同时接受“当前 generation 的旧 peer”和目标
  generation，不能把不兼容的滚动升级交给激活阶段碰运气；
- 可选 `--runtime-unit ai-secondary.service --runtime-scope system` 在更新前停止受管
  Secondary，只有 pair confirm 与最终左板 `active=target/state=Idle` 都通过后才恢复。
  runtime scope 固化在 journal 中，恢复时不允许切换；任一步失败都保持服务停止和
  journal 可续跑，不能在混合 generation 上恢复数据面。若 unit 不存在则按 inactive
  处理，不伪造“已停止/已恢复”事件。

当前可信副机已部署 updater 到
`/home/li/.local/share/makcm-ota/tools/update_proxy_pair.py`。generation 4 已是当前版本时的
真实零写续跑返回 `result=confirmed`，CH343 identity 为
`1a86:55d3/serial 5ACC041332/bus 1-7`；journal/audit 位于
`/home/li/.local/state/makcm-ota/`。自动编排专项测试为 18/18 PASS，覆盖完整三阶段、
prepared 续跑、激活失败 fail-close、旧 journal 归档、缺授权零写、已是目标版本零写及
Secondary 只在 confirm 后恢复；当前副机没有安装 `ai-secondary.service`，因此本次实机
零写复跑没有服务停启操作。

以下旧 DS-03 刷写、探测和 teardown 记录均保留为历史硬件证据；当前 generation 4
部署与复验事实以第 4 节为准。

2026-08-03 用户已明确授权并完成左板刷写。COM3 以 `VID_303A/PID_0009` 枚举为
ESP32-S3 rev v0.2、MAC `10:51:DB:43:46:8C`；`idf.py flash` 已对 bootloader、
partition table 和 359856-byte app 三段逐一返回 `Hash of data verified` 并 hard reset。
刷前 4 MiB `read_flash` 卡住且未创建文件，已终止核实的两个残留子进程后才写入，
所以没有回滚备份；不能伪称 rollback image 可用。完整审计为
`diagnostics/ds03_4131/left_flash_20260803_185942.json`，SHA-256
`f6001214a8dc62f4dca402075bde4688ff9411b9ab6fea7a8a45a3d0463bbfe9`。
刷后 `4131:3519` proxy/XInput/HID persona 已重新枚举。ROM 下载 COM3 在应用模式下
消失是预期行为；stick 控制面实际位于 UART0 GPIO43/44 的 CH343。只读远端检查已经
确认 CH343 `VID_1A86/PID_55D3` 位于 Linux 副机 `/dev/ttyACM0`，并被
`ai_secondary` PID 15494 打开；Windows 中的历史 COM5 仅是 phantom。该进程通常会把
板端从启动 115200 切到 4000000，因此不得并发抢口或假设当前仍为 115200。DS-03
接管时已核对 PID/executable 后用 `SIGTERM` 正常退出，且确认 `/dev/ttyACM0` 无 holder；
当前不得在握手结束前恢复该进程。

第一次已刷固件暴露出配置矛盾：`app_main` 把 UART0 GPIO43/44 作为 CH343 控制协议，
但当时的 `sdkconfig` 仍把 INFO console 和 coredump 指向 UART0，可能与 `STICK_*` 响应
混流。持久配置现已改为 `CONFIG_ESP_CONSOLE_NONE=y`、secondary console none、
coredump none，并增加离线回归测试。修正版 338736-byte app 已通过 IDF 5.5.5 构建并
再次刷入，
SHA-256 `ee5ed4e077eee8a749ede0d01bb448f7e010a98a128aa3e1af08c79a57abbfef`，
构建审计为 `diagnostics/ds03_4131/console_free_build_20260803_191653.json`，SHA-256
`9d9966e1e8f39dcfcc9256702698a4581b91cb96d9b6132777c810f52b4d82f3`。三段写入均
`Hash of data verified`，hard reset 后 `4131:3519` persona 重新枚举。

console-free 实机 probe 随后发现新的确定性故障：生产兼容的 115200 speed-switch 后，
4M `km.version()` 完整返回；但 `km.caps()` 单独发送只收到 `80 80 f8`，之后板端又能在
启动 115200 返回 version，证明 caps 路径发生 reset。probe 全程没有发送 output-mode、
state、rate、lease 或 move。当前最强假设是 Secondary 默认 4096-byte task stack 不足：
调用链同时保留 task RX 256 bytes、`secondaryText()` response 640 bytes，并进入 newlib
`snprintf()`；console/coredump 已按协议完整性要求禁用，因此没有 panic dump，必须以修复后
实机复测作为最终归因门禁。完整运行审计为
`diagnostics/ds03_4131/console_free_flash_caps_reset_20260803_193253.json`，SHA-256
`f28c4bb5bdcc307f9818fb34f7a652c42821dd9f6bd28488350d2e1ef73db328`。

已把该控制任务显式提高到 12288 bytes，并让 config 回归测试锁定此值；同时记录
Secondary task 历史最小剩余栈字节数并加入 `LEFT_STATUS stack=`，未观测时为 0。最终
338928-byte app SHA-256 为
`cedbc5667bff7778579e5cf4ecc1ac76d9a24628b3abad61f66c89f92c4bce28`，已使用 IDF 5.5.5
在同一 ESP32-S3/MAC 上重新刷入，bootloader/app/partition 三段均通过 hash verify 并
hard reset。

刷后 stack 修复验收不以单次成功为准：在同一 physical session 内连续运行 20 轮
`km.caps()`/`km.stick.status()`，要求 40 条响应全部严格解析、session/format/cal 不变且
没有 USB 重新枚举。随后连续查询两次 `LEFT_STATUS`；由于水位在本次 callback 返回后才
采样，使用第二次的 `stack=` 作为第一次完整 LEFT_STATUS 格式化路径的证据。第二次必须
不大于第一次且至少为 4096 bytes；0 表示尚未观测，低于 4096 表示 12 KiB 仍无足够余量，
两者都拒绝进入 arm。该阈值是控制任务栈的安全余量，不是游戏 deadzone 或整机 heap 指标。

该刷后门禁已实机 PASS：20/20 轮严格 caps/status 共 40 条响应全部解析成功，physical
session 始终为 `4132580435`，format 为 `xinput_4131_3519_v1_candidate`、cal 为 `none`，
raw-safe/source-none/zero synthetic/zero merged/fault=0；没有 USB 重新枚举。随后两次
`LEFT_STATUS` 均报告 `stack=7428`、reset reason=1，余量比 4096-byte 门槛高 3332 bytes；
命令计数从 41 增至 42，证明第二次确实位于第一次完整 LEFT_STATUS 格式化之后。完整审计
为 `diagnostics/ds03_4131/stackfix_flash_probe_20260803_195034.json`，SHA-256
`e64798e9259050c78113f9ccc54e7c5eaf81d81f4be0738cf258798b17f0a37f`。因此“4 KiB
Secondary task stack 不足”已得到修复后实机证据支持。

用户随后明确授权了“不发送 state 数据”的空载 arm。2026-08-03 20:03:47，严格 preflight
仍为 physical session `4132580435`、raw-safe、零 synthetic/merged、fault=0；`begin` 创建
injection session `1751855049` 后，没有任何符合 exact profile 的 controller report completion，
因此 delayed begin ACK 在 5 s 后超时。工具立即发送 `output_mode(mouse)`，随后独立 teardown
再次发送 mouse barrier；二者都因同一 completion 缺失而超时。全过程没有发送
`stick_state_q15`、rate、lease 或 move，`mode_` 在 mouse 命令收到时已立即停止 mutation，
但旧固件内部 injection session 仍显示 `stick_arming`。Windows XInput 连续轮询和非管理员
PnP restart 尝试均未生成 qualifying completion；后者由系统以 Access denied 拒绝，未改变
设备。`ai_secondary` 因此保持停止。主审计及恢复审计分别为：

- `diagnostics/ds03_4131/arm_20260803_200353.json`，SHA-256
  `16ad2873688c3b55ca1f59f9fde32beefc9107e22f27d38c949a2d2bb4a88cde`；
- `diagnostics/ds03_4131/teardown_20260803_200353.json`，SHA-256
  `03854c7805a3ea4e1c480342e1b47702e70861e928a26a77f22cd2a890297873`；
- `diagnostics/ds03_4131/recovery_probe_20260803_200516.json`，SHA-256
  `277f9998f3afa6e55ca10c02a733e51ea8b2974efa54eee52d311535e0fb1a5c`；
- `diagnostics/ds03_4131/recovery_teardown_xinput_20260803_200547.json`，SHA-256
  `6016152be15f5dd2faec6aa1f0bd999cf9e130a5029af9ec51a4992a847c2132`；
- `diagnostics/ds03_4131/recovery_teardown_reenum_20260803_200739.json`，SHA-256
  `241a1c3855943bfc8c7fee0de1e1c71385aec76ce95bdf940818d0fad159dbab`。

该实机故障现已形成板端修复：每个 Begin/Leave barrier 从命令接收时间起独立计时
100 ms，不依赖 host timeout；到期撤销 injection session、清 synthetic/override、回到
RawSafe/source-none、只计 `barrier_timeouts` 并排队
`STICK_NACK(begin|output_mode,barrier_timeout,raw_safe)`。Leave 超时还请求 USB detach，
冲掉可能已经提交给 TinyUSB 的旧 synthetic inflight，不能仅靠本地 disarm 假称 emission
屏障已完成。状态新增 `barrier=timeout_count/pending_code/age_ms` 与
`target=received/last_length/last_first_byte/completed`。该历史固件的离线验证记录已归档；
当前工作区重新运行的测试数量以第 4 节为准。

修正版随后通过 COM3 刷入同一 ESP32-S3/MAC；bootloader、339760-byte app 和 partition
table 三段全部 `Hash of data verified` 并 hard reset，`4131:3519` persona 正常重枚举。
刷后只读 probe 为 physical session `170063581`、raw-safe/source-none/injection=0、零
synthetic/merged、fault=0；`target=73773/20/0/73772` 证明目标流持续为 20-byte、首字节
0 且 completion 正常。空载 arm 随后收到 exact begin ACK，创建 injection session
`2484348777`，再收到 exact `output_mode(stick)` ACK；全过程没有 state/rate/lease/move。
独立 teardown 随即收到 delayed `output_mode(mouse)` ACK，最终 status 为 injection=0、
raw-safe/source-none、零 synthetic/merged、fault=0。副机已恢复原 release
`secondary-20260801-065155`，二进制 SHA-256
`ba6226a16168d81708af86d4dd39ba34e7c54d38e75121eb8797cce606e6aa52`，并重新独占
`/dev/ttyACM0`。汇总审计为
`diagnostics/ds03_4131/barrier_fix_flash_arm_20260803_202100.json`，SHA-256
`8807f2abf628920b9198a3a2710e0b67ce1c712a39b48efe257f9f618f88f006`。历史 DS-03
硬件门禁完成；当前 generation 4 已完成 OTA、20 轮只读 control-plane 与通用数据面复验，
但尚未重做 direct-stick 空载 begin/leave barrier。强制制造 no-report timeout、kill/hotplug
与 live data-plane 安全门禁仍归入 DS-04。
`preflight` 已在本机生成 `diagnostics/ds02_preflight_replug.json`。Wireshark 4.6.7 的
`tshark`、USBPcapCMD 1.5.4.0、workspace-local Python target 中的 PyUSB 1.3.1、
`libusb-package` 1.0.30.0 和 `_LibUSB` backend 均已复核通过。重启后 USBPcap
kernel service 为 RUNNING，`\\.\USBPcap1/2` 可枚举，preflight 稳定返回退出码 `0`。
目标 Xbox 风格 composite persona 在热拔插后位于 `\\.\USBPcap2`、bus 2、device
address `11`；address 不是 profile identity，每次捕获都必须重新枚举，不能写死。
10 秒 discovery 已确认 controller 候选为 `EP 0x81`、20-byte report、约 1 ms cadence；
descriptor identity 也已通过 Windows Hub IOCTL 路径确认。12-marker guided capture、
字段分析和首台设备的 protocol/integrity 人工评审均已完成；候选只被条件接受用于
phase-3 0.20 上限验证。第二台复用与 DSE identity 仍未完成，不能据此声明通用手柄
支持；游戏 plant 也仍未测量，因此不得拟合游戏曲线。

## 1. 当前暂停点与方案修订

已完成不依赖人工操作的协议、状态、codec、compose-only 接线、副机可切换输出、
V1 exact-profile/guide-candidate 工具、主机测试和两端构建。

2026-08-03 已把产品主路径明确为 `SUPPORTED_OVERLAY`：物理 controller persona
继续透传，仅在受支持的 report-format adapter 上修改右摇杆。它不要求每只手柄
中心校准，也不要求用户完成 X-/X+/Y-/Y+ 单轴扫轴。报告中心属于协议编码；需要
单独标定的是完整游戏 plant（deadzone、非线性曲线、方向和时间动态），并由
synthetic Q15 自动激励完成。

执行顺序只维护在 0.1 队列中。阶段 4 之前必须先通过阶段 3，并实现
calibration-wide 观测和三时钟对齐；当前生产中心 ROI 不能作为完整 plant 的唯一
视频证据。

现有五段单轴工具仍保留为 V1 回放/bring-up 工具，但不再定义用户操作流程。

当前 Windows 只发现 `4131:3519` 代理 persona，没有 CH343 控制串口和
`054C:0DF2` 直连节点，因此本轮没有伪造 report offset、sign、center 或 range。

## 2. 已实现

### 2.1 公共可测试核心

- `StickProtocol`：CRC16-CCITT、state/rate/lease 解析与编码、TTL 16–80 ms；
- `StickSession`：physical/injection session、uint32 半范围 seq、duplicate/stale/gap、
  source 隔离、TTL fail-center、hotplug/fault；
- `ControllerCodec`：exact identity、int16 LE/uint8、非对称 center/min/max、sign、
  Report ID/长度拒绝、compose-copy、allowed-byte mutation guard；
- `StickSynth`：axis/radial deadzone、circle clamp、physical-priority hysteresis、
  override 解除后 fresh-seq 屏障。

### 2.2 左板 IDF

- 新增 `StickRuntime`，默认 `Mouse/RawSafe`；
- 控制面：caps、begin、output_mode、profile、status、clear_fault；
- 数据面只开放 `stick_state_q15`，rate 和 move-compat 保持 disabled；
- begin ACK 等待两份 exact controller report 原样成功 completion；
- stick -> mouse/off ACK 等待一份新鲜原样 report 成功 completion；
- Begin/Leave barrier 独立 100 ms 板端 deadline；Begin 超时 disarm/raw-safe/NACK，
  Leave 超时还请求 USB detach 以冲掉可能已经 submit 的旧 synthetic inflight；
- Begin、LeaveMouse、LeaveOff 现在共用单一互斥 transition barrier；任一屏障 pending 时
  新 begin/leave/mode 命令返回 `barrier_pending`，Stick 模式内重复 begin 返回
  `leave_stick_first`，不能再覆盖旧屏障或丢失其 delayed ACK/NACK；
- phase-3 radial max 固定 0.20；
- 同轴方向反转必须成功发出一个中心 report，失败 completion 不消耗屏障；
- TTL、旧 seq、错 injection session、错 profile、CRC/frame 错误均不刷新状态；
- `snapshot/status` 使用纯读 `peek()` 计算命令年龄/逻辑过期，不再推进 session state 或
  `ttlExpiry`；真正的 USB compose 仍由 `sample()` 执行 TTL fail-center 状态转换；
- `StickRuntime` 已接入 `StickSynth::merge()`：默认 off=0.06 到 on=0.10 的
  非 latch 区间仍由 physical 原 report 优先，只有 physical 位于逻辑中心区时才写 synthetic；
- physical override latch 默认 on=0.10、off=0.06、stable=20 ms；解除后进入独立
  `awaitingFresh` 屏障并要求新 seq，等待期不再显示或计入 active override；
- active/override/expired report stream 超过 100 ms 触发 fault 和 upstream detach；
- mode/profile/session/hotplug 清 synthetic，未知 profile 永远零修改；
- modified/forced-center/override-report/awaiting-report/saturation 与 last emitted
  只在成功 completion 更新；latch/awaiting 次数和累计时长按状态转换的墙钟时间统计。
- `timing=current_seq/current_accept_ms/emitted_seq/emitted_accept_ms/emitted_ms`
  使用同一板端 monotonic clock；只有 successful USB completion 才推进 emitted seq/time，
  避免把 UART write 或 command acceptance 冒充实际输出。
- 新源码的 `STICK_CAPS` 升为 v2 并显式声明 `game_cal=0`；host 仍严格兼容读取/teardown
  当前板上 v1（v1 隐含 game calibration disabled），HIP/ADS profile 在阶段 3仅为控制标签，不代表
  plant/LUT 已生效。物理 report decode 拒绝新增 `codec_decode_error` 计数，仍保持原帧
  零修改，不放宽 exact profile 的协议量程。
- `STICK_STATUS` 统一使用 1024-byte buffer，检测 `snprintf` 截断并返回短
  `STICK_NACK(status,truncated,...)`；UART 文本发送上限同步为 1023 bytes。

### 2.3 有序 FIFO 边界

通用非鼠标 IN endpoint 仍各自使用 `EndpointContext::fifo[16]`：

```text
fifo_head 原件（只读）
  -> memcpy(compose)
    -> optional exact-profile stick mutation
      -> memcpy(inflight)
        -> TinyUSB submit
          -> delivered completion 才 fifo_head++ / fifo_count--
```

没有 controller latest-wins、没有覆写 FIFO slot、没有改变满队列丢新帧、fair drain、
submit/retry 或 completion retirement。Endpoint/FIFO/StickRuntime 仍由一个递归 task mutex
串行化，但不再用 `portENTER_CRITICAL` 把浮点 compose 和 TinyUSB submit 包在关中断区；
可选 ISR hook 仍只安排 TinyUSB task deferred drain，不取得该 mutex。静态边界门禁会检查
上述顺序和“router 不再进入 interrupt critical section”。

### 2.4 Linux Secondary

本节实现位于外部树 `C:\Users\li\ai_receiver\secondary`，不在 MAKCM 工作树；以下
“已实现”均以该外部源码和构建产物为证，不把它冒充本仓库内模块。

- 默认 `output.motion_transport: mouse_relative`，旧 `XimReportScheduler -> km.move`
  保持；
- 新增显式 `stick_state_q15` transport；
- direct path 不启动 1000 Hz mouse scheduler，不使用整数 residual；
- direct phase 3 自动禁用 XIM calibration、px/count plant、ego 和 Smith；
- 当前 `DirectStickPublisher` 仅把 legacy continuous counts 按 max-count 线性缩放成
  Q15，明确只用于阶段 3 字段/TTL/安全诊断，不是正式游戏曲线转换；
- 现有 XIM calibrator 的 `setCalibrationRight()` 可自动控制 mouse RMB，但它不是
  direct-controller persona 的 ADS trigger，不能作为 direct plant ADS 自动化证明；
- 当前 Host 在 DXGI 原始纹理上先取中心 ROI；生产配置为 `960x960 -> 640x640`，Host
  默认仍为 `640x640 -> 640x640`。Secondary 现有 XIM motion estimator 初始化搜索
  半径为 32 px（实现上限 64 px），只能承担 ROI sanity，不能证明完整 plant；
- AimAU 已把 `frame_id + t_capture_host_us` 嵌入 H.264 SEI，Secondary 也已有
  `HostToSecondaryUs()`；但当前时间戳在 `Grab()` 返回后采样，同步只有 RTT 中点的
  fixed-offset EMA，没有 DXGI `LastPresentTime`、drift fit 和 uncertainty gate，尚不
  满足动态 plant 的正式证据要求；
- 每个 control tick 发送绝对 Q15，未武装/无目标/重置时发送显式零；
- begin/output-mode ACK 解析并使用板端生成的 injection session；
- state 数据携带 seq、TTL 和 CRC；
- reconnect 必须重新握手，失败保持中心；
- 缺少 `stick_format_id` 时显式 direct 配置启动失败，不回退成猜测 profile。
- 外部 `MakcuDevice` 控制回复行容量已扩至 1023 bytes，完整 status 不会清空并污染后续
  ACK/NACK 解析。

### 2.5 DS-04 bounded state harness

- `tools/stick_phase3_state_harness.py` 只允许 CRC-protected `stick_state_q15`；明确拒绝
  rate、lease、move，并要求 `--allow-stick-output` 与
  `--allow-ds04-state-q15` 两个独立授权；
- live CLI 不自动选端口；exact firmware/entry/candidate/review 四份 SHA-256 和 Linux
  Secondary 进程 ownership guard 必须在 serial open 前通过；
- 固定 smoke 为 `zero -> X+0.05 -> zero`；full plan 覆盖 X/Y 的
  `±0.05/±0.10/±0.20` 与四个 radius=0.20 对角线，所有 nonzero 后强制 fresh-seq zero；
- host cap 使用整数 radial Q15 `<=0.20`，TTL 只允许 16–80 ms，seq 单调且拒绝 wrap；
- 每条状态必须同时证明 accepted seq、同板时钟 emitted seq/time、synthetic、physical、
  merged、state、error counters；中心附近物理漂移允许到 0.06，zero 必须保留 physical
  原 report，nonzero 才要求 merged 等于 synthetic；
- accepted→successful completion 单条门槛 `<=2 ms`，全部 latency 保存在 JSON audit；
  pending emission 只继续轮询，不能把第一次 status 当 PASS；
- 正常和异常路径都保证 explicit zero（如有必要）再 confirmed mouse teardown；最终
  injection=0、raw-safe/source-none、zeroed telemetry，否则整次失败。

### 2.6 V1 Exact profile 与 guided candidate 工具

`tools/stick_profile_tool.py` 读取五段原始 report CSV 和三份 descriptor 二进制，
验证 VID/PID/bcdDevice、endpoint、Report ID、长度、正负跨中心、单侧有效跨度、
单轴隔离和 cadence，再生成：

- 可追溯 JSON profile；
- Device+Configuration bundle SHA-256；
- HID Report Descriptor SHA-256；
- center/min/max/sign/jitter/cadence；
- profile SHA-256；
- 本地 `controller_profiles.local.inc` 编译条目。

干净源树的固件 registry 默认零条目；当前工作树已生成并包含一个 reviewed local
entry。生成条目仍须通过板端重新计算的两个 SHA-256 exact match，
interface/endpoint 检查和每帧 ID/长度检查后才能 arm。操作流程见
`docs/STICK_PROFILE_CAPTURE_WORKFLOW.md`。

旧 bring-up 路径仍接受五段单轴 CSV，并保留 10% 交叉轴门槛和 C++ 条目生成，用于
既有证据回放；它不再定义产品 qualification 操作。

`--guided-trace` 路径已实现单文件 marker 流程：initial/final neutral、物理右/上锚点、
顺/逆约两圈圆周。它自动扫描 byte-aligned `uint8`/`int16_le` 候选，使用 median、MAD、
p99/p99.9 jitter、锚点特异性、稳健 envelope、椭圆度、角度展开、圈数、方向一致性和
16-bin 覆盖评分，并排除 discrete、counter、timestamp/checksum-like、重叠或歧义字段。
交叉轴动作被允许，不再使用旧 10% 人工动作门槛。

Guided 成功只生成 `CANDIDATE_ONLY_REVIEW_REQUIRED` JSON：包含 descriptor/HID/evidence
hash、layout/sign、观测中心/jitter/envelope、协议常量假设及逐项 confidence/rejection。
它显式禁止 runtime per-device center correction；传入 `--out-cpp` 会 fail-closed，且不
生成 JSON 或 C++。协议常量仍须 DS-02 原始证据和人工评审，工具通过不等于 adapter
已经进入 firmware allow-list。

Guided 模式对 descriptor 类型显式区分：目标 interface 为 HID 时必须提供它自己的
HID Report Descriptor；目标为 vendor/XInput 时必须使用 `--no-hid-report-descriptor`，
输出 `hid_report_present=false`、SHA `null`、hash32 `0`，不得借用 composite 设备中
鼠标 HID interface 的 descriptor。Legacy C++ 发布路径仍要求真实 HID descriptor，
其旧行为没有放宽。

### 2.7 Guided USBPcap 采集前端

新增 `tools/guided_usb_capture.py`，只处理 USBPcap/Wireshark 原始 USB 证据：

- `preflight`/`doctor`：只读检查 live capture 与 descriptor snapshot 两组依赖，输出稳定
  JSON、摘要和退出码；不安装软件、不选设备/端点，也不自动开始下一阶段；
- `discover`：5–15 秒只读抓包，按 bus/device/IN endpoint/report length 汇总 cadence、
  payload 变化字节和异常，但不自动选择设备、端点或轴；
- `live`：对人工确认的 bus/device/endpoint，只导出 payload-bearing `URB_COMPLETE`，
  使用 12 个 begin/end marker 覆盖 initial/final neutral、右/上锚点和顺/逆两圈；
- `offline/inspect`：可重复处理已保存 tshark TSV，严格检查时间、端点、data_len、
  capdata、URB ID 冲突、marker 顺序/碰撞/lag；
- `descriptors`：验证统一 snapshot JSON，并按必填 `--target-interface` 原子解包
  Device/Configuration/目标 HID（若该目标确为 HID）及 hash manifest；Windows
  native HID/XInput 驱动使 libusb 视图不可靠时，使用只读 Hub IOCTL snapshot；
- live/abort 永远保留本次新建的 classic pcap 与已有 marker；成功 audit 绑定 pcap、TSV、marker 和
  输出 CSV SHA-256。CH343 wire trace、XInput/SDL 值和 submit 包均不能替代这些数据。

`discover/live` 直接严格解析 classic pcap/USBPcap pseudoheader，不依赖 Wireshark
可能被子 dissector 消耗的 payload 字段。每次启动重新枚举 device，拒绝旧文件；
Windows 独占写锁期间以新文件大小持续增长判 operational readiness，停止解锁后仍
无条件执行完整 parser。watchdog 只在 readiness 后起算，进程树受控结束。

真实 preflight、10 秒 discovery 与 12-marker live capture 已 PASS；adapter 仍是
reviewed candidate，不是已发布 allow-list。这属于 DS-02 实机取证，不影响生产
RAW_PROXY。`preflight` 的退出码固定为 `0=全部就绪`、
`3=抓包依赖缺失`、
`4=descriptor 依赖缺失`、`5=两组均缺失`。

本次 discovery 证据：

- raw pcap：
  `diagnostics/ds02_4131/discovery_replug_ready2_20260803.pcap`，
  SHA-256 `4bec7bdf3dfd4dda0a02a26b9d52426dfc04ecd87f4bb7daa1710f290d04ca78`；
- pcap 覆盖 10.158087 秒；目标 `bus 2/device 11/EP 0x81/len 20` 共 10153
  reports、3593 unique payload、8 个变化字节，cadence p50 1.000 ms、
  p95 1.007 ms；submit/error/OUT/malformed/conflict 均为 0；
- authoritative Hub snapshot：
  `diagnostics/ds02_4131/windows_hub_snapshot_replug_20260803.json`，
  SHA-256 `0ba9af861c4c60a1aabb7763be0d73a600e8db31105137175e065149f8907cc9`；
- target interface 0 是 vendor/XInput（class `0xff`），配置 146 bytes、
  `EP 0x81/0x01`、max packet 32；目标必须使用 explicit no-HID identity，
  composite 的 HID interfaces 2/3/4 不能借用。

本次 guided 与 candidate 证据：

- `guided_live_20260803.pcap` 覆盖 73.402079 秒，SHA-256
  `2fa2c293e2f548adeaa62643a2410ec9abdcec4f9e0d8b6b6c33979b73222b39`；
  audit SHA-256 `ce9ce42cd19b0915c0bea2ef507a64eae9d08caf75cc5b1005d141618c8a67b0`；
- 73384 个目标 completion、12/12 markers，全部 marker lag 小于 1 ms；guided CSV
  SHA-256 `5201f6df4d87e3009feb692dc213cdb7a0f0b3e25a57d6a8dd52b167784bddfa`；
- analyzer 的 cadence-dependent 圆面积缺陷已修复为标准 signed shoelace area；
  250 Hz 与 1000 Hz 等价轨迹得到相同 layout，同时继续拒绝同方向和不足 1.25 圈；
- candidate 文件 SHA-256
  `6b29a7728e7820c6f970004043b7349bd1a6fa86f26ce8f5098b19249aa78b9b`，
  profile SHA-256
  `dc37515bed334e67822487b9ba198ee966ac4bd00b6056a8e1e49a17637f6cba`；
  layout 为 `int16_le X@10/+1 Y@12/+1`，canonical center 0、range
  `[-32768,32767]`，decision confidence 0.967893；
- raw neutral 只能给出中心漂移/噪声，不能给出手柄或游戏 deadzone：initial neutral
  8530 reports/8.533 秒，X 为 384–512、Y 为 0，径向最大 512 counts（满量程
  1.5625%）；final neutral 1250 reports/1.249 秒，径向 p99 约 181 counts
  （0.5524%）、最大 256（0.7813%）。当前最保守的“观测中心包络”是 1.5625%，
  不是可供 `move` 自动跳过的 deadzone；
- review 文件
  `diagnostics/ds02_4131/adapter_candidate_4131_3519_20260803.review.json`，
  SHA-256 `622ee8bcd5aa195878a418f7e5129afa6715fc52bc4136de21d5ae5d0d5309d2`；
  73384 reports 中只有 bytes 6–13 变化，bytes 0–5/14–19 不随右摇杆变化，
  因此 integrity 决议为非目标字节原样 copy-through、无重算字段；stage 3 仍须实机确认。

候选自身继续保持 `CANDIDATE_ONLY_REVIEW_REQUIRED`、`firmware_cpp_emitted=false`；
人工 review 是独立证据，不能回写或伪装 analyzer 原始输出。首台条件接受不等于第二台
identity reuse PASS，也不等于 DSE adapter 已存在。

reviewed candidate 的本地 promotion 软件路径也已完成，但只授权 phase-3 0.20 测试：

- `tools/promote_stick_profile.py` 会重新计算 candidate canonical hash，校验 review 对
  candidate 文件/profile hash 的绑定、所有 evidence hash、descriptor bundle、
  interface/endpoint 拓扑、protocol/integrity 决议和 HID/no-HID 约束；失败时不生成条目；
- `controller_profiles.local.inc` SHA-256
  `21a20c5d32ddb2b964b635f45c653695157ca92372b8b2641d0548dec3839db8`，
  只含本次 exact `4131:3519/bcd0100/interface0/EP81/len20` 条目，末尾显式
  `hid_report_present=false`；HID hash/SHA 必须全零；
- registry 对旧 HID 条目保持缺省 `hid_report_present=true` 和 exact HID SHA 行为；
  no-HID 分支仍要求 descriptor SHA、endpoint/interface 全匹配，且目标 interface 不得
  出现 HID report；其他 composite HID interface 不参与目标 identity；
- 该 local entry 已进入本次 IDF build，但尚未刷板；production allow-list publication
  仍为 false。未知/不匹配 identity 继续 RAW_PROXY 零修改。

首次重启后未热拔插的旧包只覆盖 1.486727 秒；首次 readiness 修复前的新包只覆盖
7.730513 秒，二者都固定为 partial evidence，不能替代上述 PASS。

### 2.8 Trusted-Linux 自动双板 OTA 编排

`tools/update_proxy_pair.py` 已实现日常 application 更新状态机：

1. 在打开串口前读取并验证左右 `.mkota` 的 application image/hash、role、相同 target
   generation、peer generation range、生产 signer key id 和 ECDSA P-256 签名；
2. 取得单实例文件锁，记录 CH343 sysfs identity，并原子创建/恢复 0600 journal；
3. 可选停止 system/user scope 的受管 `ai-secondary.service`，等待底层工具确认串口无 holder；
4. 只查询左板本地 status 进行 downgrade/already-current 判定，不使用有维护副作用的
   standalone right status；
5. 自动执行并逐步落盘 `pair-prepare`、right-first `pair-activate`、right-first
   `pair-confirm`；进程在任何相邻步骤之间退出，重跑都会使用原 transaction 幂等续跑；
6. 最终再次要求左板 `active_generation=target`、`target_generation=0`、`state=Idle`，写出
   audit 后才允许恢复 Secondary。

updater 不负责构建或持有生产私钥。固件构建产物必须先在可信签名环境生成左右角色包；
日常 updater 只持有公钥和签名包。USB1/Windows 侧没有 OTA 检测或写入入口。

## 3. 板上计数与 gauge

`km.stick.status()` 当前包含：

- valid/invalid command、CRC/frame error；
- seq gap/stale/duplicate、wrong physical/injection session、source conflict；
- TTL expiry、profile mismatch；
- reports seen/modified/forced center；
- active physical override 次数/report 数/累计 ms；
- awaitingFresh 标志、次数/report 数/累计 ms；
- saturation、mutation guard、stream stall；
- codec decode reject；
- controller FIFO high-water/drop；
- mode switch、direction reversal center；
- last emitted synthetic/physical/merged Q15；
- mode/state/source/session/last seq/command age/profile/cal id/fault。

原有 `LEFT_STATUS` 继续提供 bridge RX/TX、endpoint submit/complete/drop/retry、
USB attach/detach/connect、control 和 mouse ledger 指标，两者不能互相替代。

## 4. 自动验证结果

- `python tests/run_host_tests.py`：19/19 PASS；
- `python -m unittest discover -s tests -p 'test*.py'`：146/146 PASS；其中自动 OTA
  编排专项 18/18 PASS；
- `python MAKCM_ESP32s3_HID_Mouse_Right/tests/test_ota_receiver_protocol.py`：4/4 PASS；
- `python tests/check_stick_fifo_boundaries.py`：PASS；
- 左板 canonical IDF 5.5.5 build：PASS；
- 右板 PlatformIO/Arduino-ESP32 2.0.16 build：PASS；
- Linux Secondary/Windows local `ai_secondary` Release build：历史外部 release 仅保留
  hash；当前源码/二进制不在本工作树，但已从明确外部路径独立重建；
- Python `py_compile`：PASS。

当前 generation 4 构建与签名产物（已实机 OTA）：

- 左板 IDF 5.5.5 app `makcm_left_idf.bin`：436064 bytes，SHA-256
  `38EC597C0494311B445231AF5DE281D8C5DF2E3EF238D61F770C9290C3AA6117`；
- 右板 application `firmware.bin`：457040 bytes，SHA-256
  `E209C54920A59402B00F6978E33C9F68F0E36047514DE25FDD486BD2B0C6BACC`；
- 左 `.mkota`：436256 bytes，SHA-256
  `C28AF5E219A8EA05E8B08E8FB7777DBE57CF09FAFFBAB1B5EFCA46FCD1A2EED7`；
- 右 `.mkota`：457232 bytes，SHA-256
  `66F4781E11369094CDFA2552D47111A7AA7B0AAF3C3BDA4981B142AC7BA45A0B`；
- 两包 manifest 均为 generation 4、peer range 3..4、signer key id
  `3dd2425f94e65ad13275fe6d1e2603bd`，离线公钥验签 `signature_valid=true`；
- 左板 Bootloader：22336 bytes，SHA-256
  `1737438B4F1AD5F585FD47B0CF7046F021CFABACE5C6962F1BCC4527668BDB5F`；
- 左板 partition table：3072 bytes，SHA-256
  `7B0775212028D27583C95F6FDED9232211BA8A8397DBA5ED05689D04C8C322D8`；
- 左板 `ota_data_initial.bin`：8192 bytes，SHA-256
  `7D2C7AC4888BFD75CD5F56E8D61F69595121183AFC81556C876732FD3782C62F`；
- 当前本地公钥信任锚 `common/ota_trust_anchor.local.inc`：SHA-256
  `175B59B2031A5EDCC3CFC13D3CF0809E512DFE300F193FEF9745B5CFCAA3BFE8`；构建已使用
  `MAKCM_OTA_TRUST_ANCHOR_CONFIGURED=1`，文件只含公钥，不含私钥；
- 当前 reviewed local controller entry：SHA-256
  `21A20C5D32DDB2B964B635F45C653695157CA92372B8B2641D0548DEC3839DB8`。

2026-08-04 审计修复后的**未刷入/未签名新源码构建**（不能冒充当前板上 generation 4）：

- 左板 `makcm_left_idf.bin`：416656 bytes，SHA-256
  `B8B384B0F2E82A01BB4B09629AD4AF82813058F6258E5B69E28FE9253E69F1E9`；IDF 5.5.5
  完整构建 PASS，最小 OTA 槽剩余 78%；
- 外部 `C:\Users\li\ai_receiver\secondary\build\Release\ai_secondary.exe`：456704 bytes，
  SHA-256 `BF07F1192ADCFE179A331F9A8729CE341EC40B5213A907ECE4F9BDDE06501CF8`；Release
  `ai_secondary` target 构建 PASS。该外部树当前没有注册 CTest；已有 test executable
  可运行，但 `ALL_BUILD` 不能作为本次 PASS 证据：`ControllerFreshnessTest` 仍引用已删除的
  `ControlOutput::soft_active`，`ai_receiver/bench_infer` 还缺 `GpuYoloPostprocess` 链接符号。
  这两项不由本次 status-line 容量修改引入，也未越界代修；
- 该新 wire schema 为 v2，增加 `game_cal=0` 和 `decode=`；host parser 同时严格接受旧
  v1/新 v2，DS-04 harness 及
  fixture 已同步。只有把它作为下一 generation 签名 OTA 部署并重做只读/barrier/数据面
  门禁后，才能替代上面的 generation 4 实机证据。

generation 3→4 实机事务 `4208040401` 已通过 Linux USB2 完成 right→left prepare、
right→left activate、right→left confirm，`pair_health_confirmed=true`；确认后左板为
`active_generation=4/target_generation=0/state=Idle`。接回 `4131:3519` 后：

- Windows XInput 9214/9214 次读取成功，右摇杆达到 `-32768..32767`，左右扳机均达到
  255，A 键 `0x1000` 被观测；
- 高速流中执行完整 `DEBUG_STATUS` 后仍保持 Streaming；板端最终只读审计覆盖
  1,237,062 个 input sequence frame，gap/duplicate/out-of-order/session-change、
  UART rxerr/txerr、endpoint retry/drop/stale 均为 0；
- generation 4 连续 20/20 轮 `km.caps()`/`km.stick.status()` 全部严格解析，physical
  session `1672789049`、profile `xinput_4131_3519_v1_candidate`、cal `none` 保持不变，
  两次 `LEFT_STATUS stack=7200`。Linux 审计为
  `/home/li/.local/state/makcm-ota/gen4-ds03-readonly.json`，SHA-256
  `53E2DEACA360F388806736BC57A9E61AF13E3CF17CDAB4AB52136AD074A40473`；工作区副本为
  `diagnostics/ds03_4131/gen4_ds03_readonly_20260804.json`，hash 相同；
- 自动更新器实机零写续跑审计已固化为
  `diagnostics/ota/gen4_pair_update_noop_20260804.json`，SHA-256
  `9F0758497EB6F4DFA2D5CD0AAF588299758105294388065A0B0AE09B727B3EF5`；事务
  `1624154591` 的事件中没有 `prepared/activated/confirmed` 写阶段事件。

历史硬件/外部产物（不作为当前待刷产物）：

- 已替代的 12 KiB control-stack + stack-watermark left `makcm_left_idf.bin`：338928 bytes，
  SHA-256 `CEDBC5667BFF7778579E5CF4ECC1AC76D9A24628B3ABAD61F66C89F92C4BCE28`；
- 已替代的首次 console-free app：338736 bytes，SHA-256
  `EE5ED4E077EEE8A749EDE0D01BB448F7E010A98A128AA3E1AF08C79A57ABBFEF`，其 caps reset
  故障仅作为回归证据保留，不再运行于板上；
- `ai_secondary.exe`（外部历史 release）：455168 bytes，SHA-256
  `4DEC02DD0213CA6386AA9D3A69EAEF33B28B457C7A6FDEA59849BDB70DB1F41C`。

## 5. 尚未执行（人工测试边界之后）

- Xbox 风格 `4131:3519` 同 identity 第二台复用验证（当前无第二台，延期到产品复用
  声明前），以及 DSE 到手后的独立 discovery/descriptor/guided qualification；
- generation 4 的签名 OTA、通用透传、20 轮只读 control-plane 与 7200-byte stack 已
  PASS；仍需在当前 generation 4 上重做“不发送 state”的空载 begin/leave barrier，
  证明 injection session 建立/撤销与最终 raw-safe/zero telemetry；
- 审计修复源码已构建但尚未刷入；下一 generation OTA 后必须重新测量控制任务最低余栈
  （status buffer 从 640 增至 1024）、完整 status、barrier 双向互斥及高负载 USB cadence，
  不能用离线构建声称新的 task-mutex/实时路径已实机 PASS；
- 自动 updater 的完整三阶段状态机已由专项测试覆盖，真实副机已通过目标版本零写续跑；
  下一次真实新 generation 发布继续使用同一入口，并保留 transaction journal/audit 作为
  非模拟 E2E 证据；不得退回日常 BOOT/ROM 刷写；
- 当前副机未安装 `ai-secondary.service`（systemd `LoadState=not-found`），且实测时
  `/dev/ttyACM0` 无 holder。若恢复常驻 Secondary，必须先安装/启用受管 system unit，或在
  更新前人工正常退出并确认 fd 释放；不得让 updater 猜 PID 或 `kill -9`；
- DS-04 same-clock timing 固件与 bounded live harness 已通过离线门禁；完成当前 gen4
  空载 barrier 后运行 0.05 smoke；
- DS-04 先以 Q15 0.20 运行字段/方向/回中/cadence/TTL 和明确零状态测试，再执行
  kill/拔 CH343/controller hotplug/physical override；每个破坏性子项都必须确认 raw-safe
  或 detach 后才继续；
- USBPcap 验证字段、方向、cadence、TTL、拔线/kill/hotplug；
- phase 3 从 0.20 提升到 0.35；
- standalone synthetic plant calibrator、AngularIntent shadow，以及 HIP/ADS、
  正负 X/Y、对角线、duration/step/ramp 的 plant 分类；
- Host `calibration-wide` 互斥全游戏画面流、geometry manifest、SEI v2/DXGI present
  timestamp、Secondary wide observer 与 Host↔Secondary↔board 三时钟仿射同步；
- adapter-declared controller ADS actuator、独立 lease/fail-release、
  CalibrationModeOrchestrator 与视觉/telemetry HIP/ADS verifier；
- V1.0 先完成 `STATIC_MONOTONIC` 完整非线性 inverse LUT；V1.1 再测试通过交叉
  验证的 `DYNAMIC_REPEATABLE` time-bucket 模型；`NON_STATIONARY` 必须拒绝
  open-loop rate；
- `stick_rate` 正式启用；
- legacy `km.move` 滑窗估计器及独立 lease；
- additive-with-fade、训练场 A/B 和 2 小时稳定性；
- bus reset、睡眠/唤醒、EP82/EP84 同窗等原代理未完成项；
- 完全不接物理 controller 的 `CANONICAL_VIRTUAL` persona（可选后续项目，非 V1
  `SUPPORTED_OVERLAY` 完成门禁）。

上述项目没有被标成 PASS；代码和构建成功不能替代实机 USB/视频证据。
