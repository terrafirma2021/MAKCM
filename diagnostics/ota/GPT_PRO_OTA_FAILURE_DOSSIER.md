# MAKCM 双 ESP32-S3 OTA 根因审计 dossier

## 委托目标

请独立审计本项目 generation 5→14 的双板 OTA 设计和实机失败证据，从根本上解释为何右板可以升级并保活，而左板每次 OTA application 首启都会回滚到 generation 5。请给出：

1. 按置信度排序的根因树，并明确区分事实、推断、待验证项；
2. 对现有双板 OTA / pair transaction / 本地 image validation 边界的架构评价；
3. 最小、完整、可审查的核心代码修复方案（优先 unified diff 或逐文件精确补丁）；
4. 不依赖手柄存在、不依赖 Windows USB1、只经可信 Linux USB2/CH343 的恢复与更新流程；
5. 单元、故障注入、断电、实机验收标准和停止条件；
6. 指出当前 Codex 尝试中哪些结论证据不足或方向错误。

不要声称运行过本地测试或访问过未提供文件。不要建议删除旧 mouse move/pass-through；Windows USB1 必须继续只表现为透传/转换器数据面，不提供刷写入口。

## 硬件与信任边界

- 左板：ESP32-S3 ESP-IDF 5.5.5，USB Device/Windows USB1，CH343 UART0 GPIO43/44 通往可信 Linux USB2。
- 右板：ESP32-S3 Arduino/ESP-IDF 4.4.7，USB Host/手柄 USB3。
- 左右板 UART1 GPIO1/2，5 Mbps。
- 更新只能从可信 Linux USB2/CH343 发起；Windows USB1 无 OTA 检测和写入命令。
- 两板均为 4 MiB Flash、A/B application；左板 `otadata @ 0xf000`，`ota_0 @ 0x20000`，`ota_1 @ 0x1f0000`，`coredump @ 0x3c0000`。
- production P-256 私钥只在 Linux，未包含在附件；固件嵌入公钥信任锚。
- 控制器不存在时必须 fail-close，但控制器/USB Host 状态不应决定本板 application 是否被 Bootloader 判死。

## 当前实机安全状态

- 左板：generation 5，Idle，target 0。
- 右板：generation 13，已确认/Idle（右板 local validation 和显式 Confirm 均成功）。
- 手柄已拔下。
- generation 14 左板失败事务已显式 Abort，未留下 target。

## 可靠事实

### 左板构建与镜像

- generation 5 运行镜像和 generation 6+ OTA 镜像都通过 `esptool image_info`，ESP32-S3 image v1、6 segments、DIO 80 MHz、secure version 0，未发现 header/segment/hash 错误。
- OTA `Prepare` 对完整 image SHA-256 验证成功；边界值、签名、公钥 key-id、role、project、partition schema 校验均通过。
- `esp_ota_set_boot_partition()` 返回 ACK，left 状态在重启前为 ActivationPending，target generation 正确。
- 左板每次失败后实际 `otadata` 曾读得：新槽 `ota_seq=5, state=ABORTED`，旧槽 `ota_seq=4, state=VALID`。这证明 Bootloader 选择过新槽，之后发生复位，第二次 Boot 将 pending image 标为 ABORTED 并回退。
- 失败后左板 status 一贯为 active generation 5、state Fault、target 指向失败 generation；显式 Abort 可恢复 generation 5 Idle/target 0。

### 右板

- generation 7/8/13 在真实 OTA 后均能 active target generation、PendingConfirm，并报告 rollback/trust/peer/health/local-validated flags。
- 新增 `kStatusLocalBootValidated=0x80` 后，右板状态 `0xFE`，证明本板 image 已取消 Bootloader rollback，即使 pair 尚未提交。
- 右板随后显式 Confirm 成功并回到 Idle；所以签名格式、Linux transport、基本 A/B/rollback 机制在右板成立。

### 主机与 journal

- updater 只信任完整 CH343 identity 和 exact transaction；unconfirmed journal 不再误晋升 confirmed。
- ACTIVATE/CONFIRM response ambiguity 不再自动 Abort。
- 多次完整 updater journal 都可靠停在 `phase=activated`：right 已 active target，left 已收到 Activate；随后 pair-confirm 超时，未伪造 confirmed。
- 典型 transaction：gen13 tx `1230944735`，prepared order right→left，activated order right→left，right active13 flags254；最终 left active5/Fault/target13，right active13/PendingConfirm。

## generation 失败时间线

- gen5：已运行基线。
- gen6：加入 quiesce release/OTA 修复。左板 image 写入、Activate 后回到 gen5 Fault；最初怀疑 host ACK-loss/自动 Abort，后来已通过工具修复排除。
- gen7：首次将 local image validation 与 pair commit 分离；右板保活，左板仍回退。
- gen8：增加 `0x80 local validated` 可观测位、STATUS 尝试触发验证；右板 `0xFE`，左板仍回退。
- gen9：临时启用 UART0 console；Activate ACK 后同一 CH343 会话切 115200 连续抓取 10 秒，0 bytes。不能据此证明无 panic，因为物理/console 路由未形成可用观测面。
- gen10：加入 app_main 早期自定义 Flash checkpoint。分区全 `0xFF`；后来证明此 checkpoint 方案本身不安全。
- gen11：checkpoint + official Flash coredump。得到部分完整内嵌 ELF；GDB 栈在 `xTaskGenericNotify → esp_ipc_call_nonblocking → spi_flash_disable_interrupts_caches_and_other_cpu → cache_disable → spi1_start`。该次 panic 是诊断代码在 app_main 太早擦写 coredump 分区所诱发，不能当作原始根因。
- gen12：移除 checkpoint，仅保留 official Flash coredump。原始失败复现；coredump partition 只写入 64-byte 外层/ELF 开头，声明长度 11876，但后续为 `0xFF`、CRC 未提交。说明 panic handler 开始写 dump 后再次中断，无法获得原始栈；高度提示崩溃上下文可能涉及 Flash/cache/IPC，但尚非证明。
- gen13：将左板 mark-valid 限制到 app_main/core0，在 Secondary/UART0 启动后、Bridge/UART1/USB/input tasks 前执行。仍回退。
- gen14：将 mark-valid 再前移到 NVS、pair store、OTA manager begin 之后、任何项目 task 创建之前；STATUS 保持只读。仍在 Activate 后约 3 秒回到 gen5 Fault。

## 已排除或显著降权的解释

- 不是串口传输太慢：两板 Prepare/hash 成功，右板同链路升级成功。
- 不是 host 自动 Abort：activate/confirm ambiguity 已禁用 safe_abort，exact status/journal 证明。
- 不是签名、role、project、partition schema 或 image header 拒绝：Prepare 和离线 image_info 均通过。
- 不是控制器/USB1 是否枚举直接导致：左板在 project tasks 创建前 mark-valid 的 gen14 仍失败。
- 不是单纯 120 秒 confirm timeout：单板实验中 Activate 后 1–3 秒即可看到旧 gen5 Fault。
- 不是只发生于 `pair-confirm`：直接 left-only Prepare/Activate/1秒 STATUS 同样回退。

## 仍需重点审计的问题

1. `OtaUpdateManager::begin()` 对 running partition state、LocalRecord、pair record 的读取/状态变换是否正确；是否可能在 pending image 首启时错误进入 Fault/rollback。
2. `esp_ota_mark_app_valid_cancel_rollback()` 的调用条件、调用返回值和后续状态是否被正确处理；当前没有持久化记录精确 `esp_err_t`。
3. Bootloader 和 application 是否确属同一 IDF ABI/rollback 配置；首次基础镜像 bootloader 是否真的对应当前 IDF 5.5.5 构建，以及仅 OTA application 更新时是否存在 bootloader/app 兼容问题。
4. 实际 `otadata` 两份 entry 的 sequence/state/CRC 演化是否与代码假设一致；是否需要在 Activate 后和首次 boot 的更细时间点读取。
5. 左板 application 可能在 mark-valid 之前/之后发生的 reset source；当前 console 不可见、原始 coredump 不完整。
6. 全局构造、main task 栈、NVS handle、FreeRTOS/IPC task control block 是否存在内存破坏；为何只有 pending-image 首启触发。
7. pair record `LeftActivated` 恢复时 `bridge.setOtaMaintenance(true)`、secondary maintenance、quiesce 等是否可能造成重启，但 gen14 在这些任务之前已尝试 mark-valid，仍需检查 mark-valid 是否真正成功。
8. 当前 sdkconfig 中 rollback、console、coredump、panic、partition table 与实际首次刷入 bootloader/partition-table 的配置漂移。

## 当前核心设计（应审计）

- `Prepare`：写 inactive application slot，逐块 CRC/offset/seq，完整 SHA-256 + ECDSA manifest 验证；不激活。
- `Activate`：`esp_ota_set_boot_partition(update_partition)`，持久化 ActivationPending record，ACK 后重启。
- pending-image boot：load LocalRecord，若 target generation/tx 匹配则进入 PendingConfirm；本地健康通过后取消 Bootloader rollback，但仍保持 pair transaction/quiesce。
- `Confirm`：只提交 pair transaction、清理 record、重启释放数据面；不应再决定本板镜像生死。
- `Abort`：若尚 pending，使用 Bootloader invalidate rollback；若已 local-valid，则显式选择 previous slot 并重启。
- updater：right prepare→left prepare→right activate→证明 right health→left activate→right confirm→left confirm→post-confirm status。

## 不可破坏边界

- 不删除 mouse move/pass-through；转换器 stick 模式未来可切换。
- 通用端点仍是有序、按 endpoint 独立 FIFO。
- Windows USB1 永远没有 OTA 命令/探测入口。
- Linux USB2 updater 不持有私钥，只消费预签名包和公钥。
- 任何错误、断线、错误 session/tx、TTL/热插拔都必须 synthetic center、USB fail-close；不得留下永久混代数据面。
- USB1/USB3 ROM 下载和 BOOT 仍作为救砖路径保留。

## 期望交付格式

1. `Root cause assessment`：事实/推断/置信度/反证。
2. `Architecture correction`：本板 image lifecycle 与 pair transaction 的明确状态图、每个持久化字段和断电恢复矩阵。
3. `Minimal code patch`：优先 unified diff，至少覆盖 left OtaUpdateManager/app_main、right OtaReceiver、host updater/tool；如不建议立即改某文件需说明。
4. `Instrumentation plan`：无需连续人工 BOOT 的持久化 reset reason、mark-valid esp_err、otadata snapshots/counters；不能在 app_main 早期随意擦写 Flash。
5. `Acceptance matrix`：单板、双板、丢 ACK、断电、错误 tx、无手柄、USB1 拔插、>120秒 pair delay、显式 rollback、重复 updater。
6. `Stop/go criteria`：哪些证据拿到前禁止继续 generation 试刷。

## 本地基线与验证

- Git root: `C:/Users/li/Documents/MAKCM`
- HEAD: `acf3d25d994b4fea8bde411ce24df69f3b4a3d7b`
- branch: `main`
- 工作区很脏；附件中的 IDF 左板、common、docs、tests、tools 多数为未跟踪或未提交状态。不得假设 HEAD 等同当前代码。
- 当前 Codex 已实际运行：Python suite 最高 162/162；OTA targeted 61/61；right protocol 8/8；左右固件均多次成功构建。实机左板 OTA 仍失败，所以测试通过不能视为完成。
