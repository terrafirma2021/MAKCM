# MAKCM 双 ESP32-S3 固件 / OTA 独立架构审计报告

**审计对象**：`MAKCM_GPT_PRO_OTA_AUDIT.zip` 冻结白名单快照  
**源码身份**：HEAD `acf3d25d994b4fea8bde411ce24df69f3b4a3d7b`，工作区 dirty；本报告只把附件内容视为本次审计基线  
**附件 SHA-256**：`275D8F03E0365BB3A3C98AA670ABBD33A4E555BBDFBA2FB84F0D46C8FAD7E87F`（已只读复核，一致）  
**审计日期**：2026-08-04  
**限制**：未运行本地构建、单元测试或实机刷写；未访问附件外源码；未接触、索取或推断生产私钥。

---

## 0. 结论先行

### 0.1 已经被证据直接证明的根因链，不再需要猜

左板 gen6–gen14 的共同失败链条是：

```text
旧槽（gen5, VALID）运行
  -> 新镜像完整写入 inactive slot，Prepare/hash/签名/角色/工程/schema 通过
  -> esp_ota_set_boot_partition() 生效，新槽被设为下一启动槽
  -> Bootloader 第一次选择新槽：NEW -> PENDING_VERIFY
  -> 新槽在成功、可读回地完成 mark-valid 之前发生复位
  -> Bootloader 下一次启动看到仍为 PENDING_VERIFY：改成 ABORTED
  -> Bootloader 排除 ABORTED 新槽，重新选择旧 VALID 槽 gen5
  -> gen5 读取残留 ActivationPending 事务，向主机报告 Fault/target=失败代
```

这不是“可能的解释”，而是 `new ota_seq=5/state=ABORTED + old ota_seq=4/state=VALID` 与 ESP-IDF rollback 状态语义共同锁定的完整因果骨架。`ABORTED` 是**第二次启动时对第一次未确认启动的后果记录**，它本身不告诉我们第一次复位是栈破坏、panic、WDT、brownout、显式重启，还是 `mark-valid` 返回错误。

旧槽保持 `VALID` 正是 Bootloader 能自动回退的原因；gen5 随后显示 `Fault` 是旧应用对残留事务的事后解释，不是导致新槽死亡的原因。

### 0.2 当前还不能诚实宣布“唯一触发根因”，但已能一次性裁决

现有附件缺少失败 gen14 的实际 ELF、map、`.su`、最终 `sdkconfig/sdkconfig.h`、Bootloader/partition-table 二进制、失败 `.mkota`/app bin，以及首次失败启动的精确 `esp_err_t`、post-mark image state 和 reset reason。因此不能把任何一个触发器伪装成已证实。

不过，源码暴露出一个明显高于其他猜想、且可在**不刷板**条件下一次裁决的左板专属问题：

- `app_main()` 中同时存在两个自动存储期 `makcm::ota::Frame`；单个 `Frame` 的字段下界已经是 4113 bytes，两个同时存活至少 8226 bytes，尚未计算对齐、保存寄存器、调用链和其他局部对象；见 `common/OtaProtocol.h:102-111`、`main/app_main.cpp:1407-1417`。
- 同一 `app_main()` 还有循环内约 1 KiB 自动缓冲区；见 `main/app_main.cpp:1506-1508`。
- `sdkconfig.defaults` 未设置 `CONFIG_ESP_MAIN_TASK_STACK_SIZE`；ESP-IDF 默认 main task 栈通常只有 3584 bytes。
- 把 mark-valid 移到“项目任务创建前”并不能排除该问题：编译器可能在 `app_main` 入口一次性预留固定栈帧；只有失败 ELF 的函数序言、map/`.su` 和最终 main-task stack 配置能裁决。

**因此当前最高价值动作不是 gen15 再换一个运行时猜想，而是先离线取得 `app_main` 的真实静态栈帧。** 若真实 frame 大于配置栈减去安全余量，这一项即被直接证明；若远小于配置栈，则立刻降权，不再围绕它试刷。

### 0.3 gen11 不能作为原始根因证据

`gen11` 在 `app_main` 极早期擦写 coredump/checkpoint Flash，得到的 `xTaskGenericNotify -> esp_ipc_call_nonblocking -> spi_flash_disable_interrupts_caches_and_other_cpu -> cache_disable -> spi1_start` 栈，是诊断代码自己触发的 Flash/cache/IPC panic。它只能证明“那种诊断方式不安全”，不能证明 gen6–gen10 的原始故障也来自同一调用链。`gen12` 仅留下 64-byte coredump 外层/ELF 开头、后续全 `0xFF`，说明 panic dump 开始后又中断；这是内存/栈/Flash 上下文异常的弱提示，不是原始 PC/backtrace。

### 0.4 必须立即改变的架构原则

1. **本板镜像存活与双板 pair commit 完全解耦。** 新应用一旦完成最小本地健康检查，就独立取消 Bootloader rollback；主机失联、控制器缺失、USB1 未枚举、UART1 peer 未就绪或 pair 延迟超过 120 秒，只能让数据面保持 fail-close，不能杀死本板可启动镜像。
2. **Confirm 不再决定镜像生死。** Confirm 只写 pair-completed tombstone、释放维护隔离；不得在 Confirm 中首次调用 mark-valid。
3. **板端事实必须包含精确事务和镜像身份。** 空闲板不能仅把主机请求的 tx 原样回显，主机也不能仅凭 active generation/flags 推断“原事务已完成”。
4. **任何改变启动选择的命令都必须先持久化 intent，再执行 Bootloader 操作。** 当前左右板均为 `set_boot -> persist`，存在断电不可解释窗口。
5. **不再用新 generation 充当日志系统。** 在拿到一次性裁决证据前，禁止继续刷下一代。

---

# A. 根因树与排序

## A1. 证据分级方法

- **事实 F**：附件中的源码、冻结 dossier 或已解码 Flash 状态直接给出；无需依赖额外假设。
- **高/中/低置信度推断 H/M/L**：能解释事实，但仍缺一个决定性观测。
- **已反证 R**：与已知状态或时间线冲突，不能再作为主要解释。
- **最小裁决证据 D**：只增加一条证据即可使该分支成立或退出根因树。

置信度为审计排序，不是统计概率。

## A2. 直接证明的事实

| 编号 | 事实 | 证据 |
|---|---|---|
| F1 | 左板 gen6–gen14 的新槽至少被 Bootloader 选中过一次，随后未完成确认并被标为 `ABORTED`；旧 gen5 保持 `VALID`。 | dossier `35-41` |
| F2 | Prepare 已验证完整 image SHA-256、边界、P-256 manifest 签名、key-id、role、project、partition schema；`esp_ota_set_boot_partition` 路径返回 ACK，重启前状态为 ActivationPending。 | dossier `35-40` |
| F3 | left-only Prepare/Activate 同样在约 1–3 秒内回退，故障不依赖 pair-confirm，也不是 120 秒超时。 | dossier `67,75-76` |
| F4 | 右板 gen7/8/13 的本地 validation 和显式 Confirm 成功，`0x80 local validated` 被观察到。 | dossier `43-47` |
| F5 | 主机已取消 ACTIVATE/CONFIRM 响应不明时的自动 Abort；journal 多次停在 activated，没有伪造 confirmed。 | dossier `49-54`；`makcm_ota_tool.py:1282-1313` |
| F6 | gen11 的 Flash/cache/IPC panic 由早期擦写诊断诱发；不能外推为原始故障。 | dossier `63-65` |
| F7 | gen14 的 mark-valid 调用点位于 NVS、pair store、OTA manager begin 之后、项目 task 创建之前，但源码没有记录 mark API 的精确返回值和写后 image state。 | dossier `66-67`；`ota_update_manager.cpp:467-480` |
| F8 | `validateLocalBoot()` 把所有非 `ESP_OK` 压缩成 `false`，没有持久化 `esp_err_t`、没有立即 `esp_ota_get_state_partition()` 回读 `VALID`。 | `ota_update_manager.cpp:467-480` |
| F9 | 左板 `app_main()` 有两个同时存活的 4 KiB 级 `Frame` 自动对象；defaults 未提高 main task 栈。 | `OtaProtocol.h:102-111`；`app_main.cpp:1407-1417`；`sdkconfig.defaults:1-56` |
| F10 | 左板 LocalRecord 只有 state/subtype/tx/target generation/CRC；没有镜像 hash、previous slot、running/boot slot、reset、API error、完成墓碑或 sequence。 | `ota_update_manager.h:63-74` |
| F11 | pending boot 的 `begin()` 只核对 record target generation 和非零 tx；不要求 record phase=ActivationPending，不核对 recorded slot==running slot，也不核对运行镜像 hash。 | `ota_update_manager.cpp:61-83` |
| F12 | 左板 `activate()` 与右板 `handleActivate()` 都先 `esp_ota_set_boot_partition()`，再持久化 Activated/ActivationPending。 | left `420-441`；right `529-547` |
| F13 | 右板 Preferences 用多个 key 分别写入且不检查每次 put/clear 的结果；重启时 Prepared 被映射成 Failed。 | `ota_receiver.cpp:277-321` |
| F14 | 当前响应协议不携带“板端持有的 tx/image hash/completed tombstone”；右板无记录时会用请求 tx 回应。主机只过滤 transaction/target，不校验 response sequence 和 `last_command`。 | `OtaProtocol.h:89-98`；`ota_receiver.cpp:214-275`；`makcm_ota_tool.py:985-1052` |
| F15 | pair record 的 previous-left/right generation 在首次保存时都写成当前编译 generation，而不是两板实际状态；恢复动作多数只计算，未执行。 | `app_main.cpp:689-712,966-984` |
| F16 | updater journal 只有全局 `new/prepared/activated/confirmed`；一个 pair 子进程完成后才落一个阶段，不能表达每块板、每条命令的 intent/fact。 | `update_proxy_pair.py:45-89,701-762` |
| F17 | 左板分区表内部无重叠：NVS 0x9000、otadata 0xf000/0x2000、ota_0 0x20000/0x1D0000、ota_1 0x1F0000/0x1D0000、coredump 0x3C0000；粗粒度偏移错误不符合“新槽已被选中并进入 ABORTED”。 | `partitions.csv:4-9` |
| F18 | dossier 的 app image header 为 DIO 80 MHz，而 `sdkconfig.defaults` 写 QIO 80 MHz；因此 defaults 不是最终生成配置的可靠替代品。 | dossier `37`；`sdkconfig.defaults:7-8` |

## A3. 完整因果解释：为什么是 1–3 秒 ABORTED，而旧槽仍 VALID

1. gen5 在旧槽运行，旧槽在 otadata 中为 `VALID`。
2. 新镜像写入 inactive OTA slot，并由 `esp_ota_end`/Prepare 流程验证；随后 `esp_ota_set_boot_partition(new)` 更新 otadata，将新槽设为一次性首启候选。
3. 重启时，Bootloader 把新槽的 `NEW` 转为 `PENDING_VERIFY` 并启动它。
4. 在新应用成功调用 `esp_ota_mark_app_valid_cancel_rollback()`、该调用返回 `ESP_OK`、且写后状态可读为 `VALID` 之前，发生了一次复位。
5. 下一次 Bootloader 启动看到上一次候选仍为 `PENDING_VERIFY`，将其变为 `ABORTED`；`ABORTED` 不再可选。
6. Bootloader 选择仍然 `VALID` 的旧 gen5。
7. 旧 gen5 看到 NVS 中尚有 ActivationPending，进入 Fault 并保留 target，方便显式 Abort/诊断。

这里的“1–3 秒”只约束触发器发生在首次启动早期；它不能区分 panic、WDT、brownout、栈破坏或应用显式 `esp_restart()`。由于显式 rollback API 应把当前镜像标成 `INVALID`，而现场读到 `ABORTED`，现有证据更符合**未成功确认后发生普通复位**，而不是成功执行显式 invalidate。

## A4. 候选触发根因排序

### 1. `app_main` 静态栈帧过大 / 早期内存破坏

- **置信度：高。**
- **支持证据**：
  - 两个同时存活的 `Frame` 自动对象的最小字段总量已经超过 8 KiB；默认 main task stack 为 3584 bytes。
  - `CONFIG_COMPILER_OPTIMIZATION_PERF=y`，真实函数序言可能一次性预留整个固定 frame；把 mark-valid 的源码语句前移并不保证它在栈破坏之前执行。
  - gen12 coredump 只写头部后中断，与栈/TCB/内存已损坏相容。
  - 这是左板当前源码独有，而右板把大帧放在对象成员/静态区，符合“右成功、左失败”的差异。
- **反证/降权因素**：
  - 附件没有最终 `sdkconfig`；实际 main stack 可能已被其他配置提高。
  - 编译器可能进行分支局部栈调整或消除部分对象；不能只按源码相加宣布已证实。
  - 历史 gen4/5 可运行，但这些局部对象可能是后续代引入，附件无法重建每代差异。
- **决定性验证 D1（不刷板）**：
  1. 取得失败 gen14 的最终 `build/config/sdkconfig.h`，读取 `CONFIG_ESP_MAIN_TASK_STACK_SIZE`。
  2. 对同一失败 ELF 执行 Xtensa `objdump -drwC -S`，检查 `<app_main>` 函数序言的 SP 调整；同时检查 map 和 `-fstack-usage` 生成的 `.su`。
  3. 若 `app_main static frame + 至少 1024 bytes 调用余量 > configured main stack`，即直接判定根因成立；若有不少于 2 KiB 余量，则该分支退出首位。

### 2. `esp_ota_mark_app_valid_cancel_rollback()` 或 otadata 重写失败

- **置信度：中高。**
- **支持证据**：
  - 精确 `esp_err_t` 未记录；API 会读取 active otadata、擦除其中一个 sector、写回 `VALID`，任何 read/erase/write 错误都会返回给调用者。
  - 没有写后 `esp_ota_get_state_partition(running)`，所以当前无法证明 mark 生效。
  - 左板 gen11/12 的 Flash/cache/IPC 痕迹虽然不能证明原始根因，但使“Flash 操作失败或被内存破坏影响”仍应保留。
  - final sdkconfig、Bootloader 和 flash 上实际 partition table 缺失。
- **反证/降权因素**：
  - 同一设备多次 `esp_ota_set_boot_partition` 能成功改写 otadata；并非整个 otadata 完全不可写。
  - mark-valid 是 ESP-IDF 标准首启路径，单纯“在 app_main/core0 调用”不应天然失败。
- **决定性验证 D2**：固定二进制探针记录 `pre get_state err/state -> mark err -> immediate post get_state err/state`。若 mark 返回错误，错误码直接裁决；若返回 `ESP_OK` 且 post-state=`VALID`，此分支作为直接原因被反证，必须转查后续 otadata 被谁重写或运行的并非预期镜像。

### 3. 失败 `.mkota` 的 generation 与镜像内生身份不一致

- **置信度：中。**
- **支持证据**：
  - manifest 的 generation 被签名、image hash 也被签名，但“该 image 自己声明的编译 generation”没有在 Prepare 时核对。
  - 左板只验证 app descriptor `project_name`；一个旧 bin 完全可以被重新包装为 gen14，签名仍合法。
  - pending boot 的 `begin()` 要求 `record.target_generation == compiled kPairGeneration`；若包标 14、镜像实际编译常量不是 14，它会进入 Fault、不会 mark-valid，下一次普通复位便形成 ABORTED。
- **反证/降权因素**：
  - 没有附件中的失败 package/bin 可以证明真的发生了错包。
  - 右板包生成正确不能证明左板包路径正确，但也说明签名工具整体并非完全失效。
- **决定性验证 D3（不刷板）**：对失败 `.mkota` 解包，核对 manifest image SHA、app bin SHA、`esp_app_desc.version/project_name/app_elf_sha256`、失败 ELF 中 `kPairGeneration` 符号/反汇编常量及 package generation。任何不一致即直接定案。

### 4. 实际 Bootloader / application rollback 配置或 ABI 漂移

- **置信度：中。**
- **支持证据**：
  - 附件只有 `sdkconfig.defaults`，没有最终 `sdkconfig.h`、Bootloader binary、flash args。
  - image_info DIO 与 defaults QIO 已证明存在生成配置/默认文件差异。
  - 初始 Bootloader 可能来自较早 IDF，而只 OTA application 到 IDF 5.5.5。
  - 理论上可能出现“Bootloader 启用 rollback，而新 application 编译时 `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` 关闭”，导致新应用根本不编译 mark 路径。
- **反证/降权因素**：
  - otadata 的 NEW/PENDING/ABORTED 演化证明当前 Flash 上 Bootloader 的 rollback 路径确实启用。
  - gen4 曾有一次真实双板 OTA 成功，显著降低“固定偏移或 Bootloader 设计从来错误”的概率；但不能证明当前 flash bytes 与当时相同。
- **决定性验证 D4**：取得失败构建的 generated config/Bootloader/table/flash args；只读 dump 当前板 `0x0..bootloader`、partition table、otadata，并核对 hash。确认 application 与 Bootloader 都启用 rollback、目标 chip/IDF 兼容、表偏移一致。

### 5. NVS / LocalRecord 恢复门错误，导致 mark-valid 根本未被调用

- **置信度：中低。**
- **支持证据**：
  - `nvs_flash_init()` 只接受 `ESP_OK`，不记录其他错误；任何 NVS 初始化失败都会令 `ota_ready=false`，从而跳过 mark。
  - `begin()` 对 pending image 的 record 只做极少校验；错误 generation 会进入 Fault。
  - LocalRecord 没有 image hash、record sequence、previous slot；clear 的返回值被吞。
- **反证/降权因素**：
  - Activate ACK 发生在左板 saveRecord 成功之后，因此至少旧镜像写入 ActivationPending 的那次 NVS 操作大概率成功。
  - record mismatch 本身不会在 1–3 秒自动重启；还需要另一个 reset 触发器。
- **决定性验证 D5**：首启只读记录 `nvs_flash_init/nvs_open/nvs_get_blob` 的精确返回值、record 原始字段/CRC、running/boot subtype、compiled generation。若这些在 mark 前不匹配，直接裁决。

### 6. 首启 panic / WDT / brownout / 电源问题

- **置信度：低到中。**
- **支持证据**：ABORTED 必然需要一次未确认复位；现有 console/coredump 没有给出 reset reason；1–3 秒符合某些 WDT/panic。
- **反证/降权因素**：右板同电源链路成功；左板旧槽长期稳定；故障与新应用版本高度相关，更像软件差异。
- **决定性验证 D6**：在新槽入口实时输出并在 RTC retention 保存 `esp_reset_reason()`、最后 checkpoint、PC/exception cause；回退后的旧槽在任何人工复位前读取并回传。若为 brownout，再补电源轨示波/逻辑采集；不应先假定。

### 7. 分区偏移、gross image header、custom 签名、native Secure Boot / anti-rollback

- **置信度：低。**
- **支持证据**：最终生成物/eFuse 摘要缺失，因此不能形式上完全排除 native Secure Boot/flash encryption/secure version 配置。
- **反证**：Prepare、image_info、`esp_ota_set_boot_partition` 和 Bootloader 选择新槽均已发生；gross offset/header/custom signature 错误通常会更早失败。secure version 为 0 且 set_boot 成功，也显著降低 anti-rollback 拒绝的概率。
- **决定性验证 D7**：只读、脱敏的 eFuse security summary（只保留 Secure Boot enabled、Flash Encryption enabled、secure version、download/JTAG policy，不读取任何 key block）；同时核对 generated sdkconfig 与 image signature metadata。绝不需要私钥。

### 8. host Abort、pair-confirm、控制器、USB1、120 秒超时

- **置信度：作为直接根因为极低，主要假设已反证。**
- host 在 ACTIVATE/CONFIRM ambiguity 已不自动 Abort；journal 停在 activated。
- left-only 仍失败。
- 回退发生于 1–3 秒，不是 120 秒。
- gen14 在项目任务创建前已尝试 local validation；控制器/USB1 不能解释所有代。

## A5. 已被反证或不应再作为“下一代猜想”的假设

| 假设 | 审计结论 |
|---|---|
| 串口太慢/传输出错 | 已被完整 SHA、Prepare 成功和右板同链路成功反证。 |
| ACTIVATE ACK 丢失后 host 自动 Abort | 当前工具已移除该行为，实机仍复现；不是 gen13/14 根因。 |
| manifest 签名、role、project、schema、普通 app header 失败 | 都在 Prepare/离线 image_info 阶段通过；不是最早拒绝点。 |
| 只因 controller 不在或 USB1 未枚举 | left-only、任务前 mark 尝试仍失败；至多是后续数据面条件。 |
| 单纯 pair Confirm 120 秒超时 | 1–3 秒回退，且 left-only 复现。 |
| gen11 Flash/IPC backtrace 就是原始故障 | 明确由诊断擦写诱发，必须剥离。 |
| “mark-valid 源码行已前移，所以任何应用代码都不可能先出错” | 被 `app_main` 固定栈帧、全局构造、NVS/manager 代码和系统启动上下文反驳。 |
| “测试 162/162、固件能构建，所以 OTA 完成” | 实机连续失败已直接反证；现有测试主要是模型/源码字符串/host mock。 |

## A6. 一次即可裁决的最小证据包

### A6.1 第零阶段：完全不刷板的离线证据

Codex 本地只需冻结并输出以下文件及 SHA-256：

```text
失败 gen14 的：
  *.mkota
  application.bin
  application.elf
  application.map
  build/config/sdkconfig.h（或最终 sdkconfig）
  bootloader/bootloader.bin
  partition_table/partition-table.bin
  ota_data_initial.bin（若有）
  flash_project_args / flasher_args.json
  编译器、ESP-IDF commit/version、完整构建命令
```

只读检查命令示例（路径按本地实际构建树替换）：

```bash
sha256sum <上述文件>
grep -E 'CONFIG_(ESP_MAIN_TASK_STACK_SIZE|BOOTLOADER_APP_ROLLBACK_ENABLE|BOOTLOADER_APP_ANTI_ROLLBACK|PARTITION_TABLE|ESPTOOLPY_FLASHMODE|ESPTOOLPY_FLASHFREQ|ESP_COREDUMP|FREERTOS_UNICORE|SECURE_BOOT|FLASH_ENCRYPTION)' build/config/sdkconfig.h
xtensa-esp32s3-elf-objdump -drwC -S build/makcm_left_idf.elf > gen14.objdump.txt
xtensa-esp32s3-elf-nm -S --size-sort build/makcm_left_idf.elf > gen14.nm.txt
esptool.py --chip esp32s3 image_info --version 2 build/makcm_left_idf.bin
```

对同一 source/config 额外生成一次 **不用于刷写** 的 `-fstack-usage` 构建，收集 `app_main.cpp.su`。这不是“又做一个固件猜想”，而是离线编译证据。

### A6.2 第零阶段的硬停止条件

以下任一缺失，**禁止签名/刷写 gen15**：

- 无法证明待刷 package 内 image SHA 与冻结 app bin 一致；
- 无法证明 ELF 中编译 generation 与 manifest generation 一致；
- 无法取得最终 main-task stack 配置和 `app_main` frame；
- 无法证明 Bootloader/table 与当前板 Flash 的只读 hash 对应；
- 修复固件仍在 `app_main` 保留 4 KiB 级自动对象；
- 首启探针仍要在 mark-valid 前擦写 coredump/checkpoint/raw Flash。

### A6.3 若离线证据仍不能唯一裁决：只允许一版固定探针

该探针必须同时做到：

1. 所有大对象移到 `.bss`/静态对象；`CONFIG_ESP_MAIN_TASK_STACK_SIZE=12288` 作为保守临时值，之后用 `.su + high-water mark` 收敛。
2. 不创建 USB/UART1/input 项目任务；只初始化 UART0 明确 GPIO43/44、115200，使用固定长度二进制 checkpoint，不调用 `printf/snprintf`。
3. mark-valid 前除 Bootloader 必需的 otadata API 外，**不擦写任何诊断 Flash**。
4. 用 `RTC_NOINIT_ATTR` 双副本（sequence+CRC）保存软件复位可保留的 checkpoint；同时实时发到 Linux USB2。仅在 mark 成功且 post-state=`VALID` 后，才把完整结果写入 NVS 事务记录。
5. 依次记录：

```text
P0 app entry
P1 reset_reason + RTC boot counter
P2 running partition addr/subtype + boot partition addr/subtype
P3 pre esp_ota_get_state_partition() err/state
P4 nvs_flash_init/open/load exact err + record fields/CRC
P5 compiled generation + running image SHA + record image SHA comparison
P6 before mark
P7 mark esp_err_t
P8 immediate post get_state err/state
P9 5 秒纯空闲，无项目 task
P10 准备启动 Secondary（第一轮不实际启动也可）
```

结果裁决：

| 观测 | 结论 |
|---|---|
| 没有 P0 | image/Bootloader/全局构造/函数序言/极早栈问题；先看 ELF 与 Bootloader log。 |
| 有 P0，无 P4/P6 | NVS/record/early stack 或初始化路径。 |
| P6 后 mark 返回非 OK | 直接按 `esp_err_t` 处理 otadata/flash/config；不再猜。 |
| mark=OK，但 P8 不是 VALID | otadata/ABI/flash 异常，立即只读 dump 两个 otadata sector。 |
| P8=VALID，之后现场仍出现该槽 ABORTED | 普通解释已不成立：必须有后续 otadata 改写、运行镜像身份错位或错误 Flash 读证据；禁止再刷。 |
| P8=VALID 且 5 秒稳定 | 本板存活边界已跨越；后续 task 逐组二分启用，不增 generation，不改变探针记录。 |

---

# B. 纠正后的 OTA 架构

## B1. 三个状态机必须分离

### B1.1 Bootloader 状态机（ESP-IDF 所有）

```text
UNDEFINED/VALID
   -> set_boot(target)
NEW
   -> first boot
PENDING_VERIFY
   -> app local validation OK: VALID
   -> explicit local failure: INVALID + rollback
   -> reset/power loss/crash before validation: next boot ABORTED + rollback
```

Bootloader 只知道“单颗芯片上的 app slot 是否可启动”，不知道双板 pair、控制器、USB1 或 Linux journal。

### B1.2 每板 Local Image Lifecycle

```text
EMPTY
  -> RECEIVING
  -> PREPARED
  -> ACTIVATE_INTENT          (intent 已持久化，boot selection 尚未必改变)
  -> BOOT_SELECTED            (set_boot 已成功)
  -> BOOT_PENDING             (目标槽正在 PENDING_VERIFY 首启)
  -> LOCAL_VALIDATED          (mark=OK 且 post-state=VALID)
  -> PAIR_PENDING             (镜像安全存活，但数据面隔离)
  -> PAIR_COMMITTED           (pair tombstone 已完成，可释放数据面)

任一点可进入：
  ROLLBACK_INTENT -> ROLLED_BACK
  FAILED_EVIDENCE_PRESERVED
```

**LOCAL_VALIDATED 之后不允许因为 120 秒、主机失联、peer/controller/USB1 缺失而本地 rollback。** 这些条件只控制 `PAIR_PENDING` 的维护隔离。

### B1.3 Pair Transaction Lifecycle（由左板协调，主机编排）

```text
NONE
 -> RIGHT_PREPARED
 -> BOTH_PREPARED
 -> RIGHT_ACTIVATE_INTENT
 -> RIGHT_LOCAL_VALIDATED
 -> LEFT_ACTIVATE_INTENT
 -> BOTH_LOCAL_VALIDATED
 -> PAIR_COMMIT_INTENT
 -> RIGHT_PAIR_COMMITTED
 -> BOTH_PAIR_COMMITTED / COMPLETED_TOMBSTONE
```

Pair commit 的必要条件是两板：

- exact transaction 相同；
- exact target generation 相同；
- exact image SHA 分别匹配已签名 package；
- running slot 与记录 target slot 相同；
- Bootloader image state=`VALID`；
- local validation flag 已持久化；
- protocol/schema/peer range 兼容。

**控制器存在不是 pair commit 条件。** 控制器缺失时更新仍应成功，正常数据面保持“无设备/fail-close”，待控制器接入后再走 descriptor/profile/health 门。右板 USB Host driver 能安装是本地运行健康的一部分，但“当前有控制器并完成枚举”不是 app 生死条件。

## B2. 持久化字段

左右板统一使用单一 CRC blob，禁止右板多个 Preferences key 拼成非原子记录。建议 A/B 两个 NVS key（`ota_rec_a`、`ota_rec_b`）交替写入，按 `record_seq + CRC` 选最新有效项。

```cpp
struct OtaRecordV2 {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t record_seq;

    uint8_t  role;
    uint8_t  local_phase;
    uint8_t  target_subtype;
    uint8_t  previous_subtype;
    uint32_t transaction_id;
    uint32_t target_generation;
    uint32_t previous_generation;
    uint32_t partition_schema;

    uint32_t image_size;
    uint8_t  image_sha256[32];
    uint8_t  manifest_sha256[32];
    uint8_t  running_image_sha256[32];
    uint32_t min_peer_generation;
    uint32_t max_peer_generation;

    uint8_t  running_subtype;
    uint8_t  boot_subtype;
    uint8_t  last_image_state;
    uint8_t  local_validated;
    uint8_t  pair_committed;
    uint8_t  last_ota_op;
    uint16_t boot_attempt;
    int32_t  last_esp_err;
    int32_t  last_state_query_err;
    uint32_t last_reset_reason;

    uint32_t last_request_sequence;
    uint8_t  last_request_command;
    uint8_t  last_response_command;
    uint16_t last_nack_reason;

    uint32_t completed_tx;
    uint32_t completed_generation;
    uint8_t  completed_image_sha256[32];
    uint32_t crc;
};
```

说明：

- `image_sha256` 来自签名 manifest；首次启动对 running partition 做只读 SHA，必须相等。
- `compiled generation` 还必须等于 `target_generation`；建议同时把 generation 编进 `esp_app_desc.version`，Prepare 阶段即可拒绝错包。
- `previous_subtype` 在 Activate 前从实际 running partition 保存；禁止用 `esp_ota_get_next_update_partition(nullptr)` 猜 previous。
- `completed_*` 是完成墓碑。Confirm 后不能直接 `clear()`，至少保留最近一次 exact tx/gen/image，以便 ACK 丢失时证明已完成。
- chunk 级 NVS journal 第一版可不做：Receiving 断电后从 0 重传；但 Prepared 必须可恢复。

左板另有 `PairRecordV2`：

```text
record_seq, tx, stage,
left/right previous generation + previous slot,
left/right target generation + image SHA + target slot,
left/right local_validated facts,
left/right pair_committed facts,
last command/error,
completed tombstone, CRC
```

不得再把 previous-left/right 都写成当前编译 generation。

## B3. 不变量

1. `ACK(ACTIVATE)` 只能在 `ACTIVATE_INTENT` 已持久化且 `set_boot` 返回 OK 后发送。
2. `ACK(PREPARE)` 只能在 image hash、signature、app descriptor generation、target slot 和 `PREPARED` record 全部持久化后发送。
3. 新槽只有在 `running==record.target slot`、`boot==record.target slot`、compiled generation/hash 匹配且最小本地健康通过时才能 mark-valid。
4. mark-valid 成功必须定义为：`mark_ret==ESP_OK && post_get_state_ret==ESP_OK && post_state==VALID`；缺一不可。
5. `LOCAL_VALIDATED` 是单调状态，pair 延迟不能将其退回 pending/invalid。
6. state-changing command 的 exact `(tx, command, sequence, image identity)` 重放必须返回相同结果；同 tx 不同 payload/hash、旧 tx、错 generation 必须 NACK 且零副作用。
7. 响应必须 echo 请求 sequence；主机同时校验 `sequence + status.last_command + tx + target`。
8. STATUS header tx 只用于关联请求；payload 必须单独返回 `record_tx` 和 `completed_tx`，禁止把请求 tx 伪装成板端事实。
9. rollback/confirm 前先写 intent；成功后写 fact/tombstone；任何 evidence 不得在操作完成前清除。
10. Host journal 是编排记录，不是硬件事实；只有精确板端 record/tombstone 才能推进或修正 journal。
11. 数据面只能在 `BOTH_PAIR_COMMITTED` 后释放；任何混代、identity 不明或 record 腐坏都保持 synthetic center、USB fail-close。

## B4. 关键断电点及确定性恢复

| 断电点 | 重启后的确定动作 |
|---|---|
| RECEIVING 中 | 旧 VALID 槽运行；没有 chunk journal 时丢弃未完成 handle，保留 Failed/Receiving evidence，要求同 tx 从 Manifest/offset 0 重传。不得误报 Prepared。 |
| image 完成、PREPARED record 写前 | 旧槽运行；无 Prepared 事实，主机重做 Prepare；重复完整写安全。 |
| PREPARED record 写后 | 恢复 Prepared，target slot/hash/tx 可证明；等待或幂等重试 Activate。 |
| ACTIVATE_INTENT 写后、set_boot 前 | 旧槽运行；比较 boot partition 仍为旧槽，保持 Prepared/ActivateIntent；同 tx Activate 重试 set_boot。 |
| set_boot 成功、BOOT_SELECTED/ACK 前 | 旧槽可能仍运行或即将重启；从 boot partition + intent 可重建 BootSelected；不得 Abort 猜测。 |
| target 首启、mark 前 | 目标读取 intent/identity并立即本地验证；若崩溃，Bootloader 回退，旧槽根据 record+otadata 固化 FailedRollback 证据。 |
| mark 返回 OK、post-state 前 | 首先只读回查；若重启，下一次 target/old app 以 otadata VALID 为权威重建 LocalValidated。 |
| LOCAL_VALIDATED record 前 | otadata VALID 是权威；重启后 target 将 record 补写为 LocalValidated。 |
| LOCAL_VALIDATED 后主机失联 | 永久保持目标槽和维护隔离，等待 exact tx 恢复；不按 120 秒回滚。 |
| 仅右板 LOCAL_VALIDATED | 左板/主机读取右精确事实，继续左 Activate；不得重新写右镜像。 |
| 两板 LOCAL_VALIDATED、Confirm 前 | 写 PairCommitIntent，再逐板写 pair tombstone；数据面仍关闭。 |
| 右 Confirm 成功、ACK 丢失 | 右 completed tombstone 证明完成；重试不再改变状态，继续左 Confirm。 |
| 左 Confirm 成功、journal 未保存 | 两板 completed tombstone + image identity 推进 host journal confirmed。 |
| ROLLBACK_INTENT 后断电 | 记录保留 exact previous slot；启动时继续 rollback，直到 old slot running 且 image valid 后写 RolledBack tombstone。 |
| 两槽都坏 | application 级恢复不可能；保持 ROM/BOOT 物理救砖。正常 Windows USB1 固件永远不暴露刷写接口。 |

## B5. USB1 / USB2 / 普通数据面的隔离

### Windows USB1

- 正常固件只提供透传/转换器数据面；不得出现 CDC、DFU、MSC、vendor maintenance interface 或可被 Windows report/control request 触发的 OTA parser。
- `MKOT` 解析入口只能由 `Secondary` UART0/CH343 调用；USB control/HID/通用端点 callback 不得引用 `OtaUpdateManager::handle`。
- USB1 拔插只影响数据面枚举，不改变本地 image lifecycle、pair record 或 Bootloader state。
- 物理按住 BOOT 进入 ROM 下载属于救砖，不是运行时 USB1 暴露的维护能力；应在操作规程和权限上与日常 updater 分离。

### Linux USB2 / CH343

- 唯一日常 updater 入口；继续 pin VID/PID/serial/bus path/tty，独占串口。
- updater 只消费预签名 package 和公钥；不接受私钥，不调用 ROM/esptool，不更新 Bootloader/table/otadata。
- 右板 OTA 仍由左板在维护态经 UART1 转发。

### 普通 HID/透传与 OTA maintenance

进入维护前：

1. synthetic center / release buttons；
2. 停止 mouse/stick injection；
3. 停止普通 UART1 report/log traffic；
4. 若 USB1 已枚举，主动 disconnect 并等待 bounded detach；
5. UART1 切换为严格 MKOT binary ownership；
6. 完成上述条件后才 ACK EnterMaintenance。

维护退出只在双板 pair tombstone 完整、无旧 session/TTL、状态一致后发生。controller 不存在时 pair 可完成，但数据面仍等待新设备；controller 存在时必须测试 teardown 和重新枚举，不得把其存在作为 local mark-valid 条件。

## B6. Bootloader、application、updater 的职责边界

| 层 | 必须负责 | 不应负责 |
|---|---|---|
| Bootloader | A/B 选择、otadata 双 sector、NEW/PENDING/VALID/ABORTED/INVALID、native Secure Boot/anti-rollback、坏槽 fallback | pair transaction、controller、USB1、host journal、custom P-256 package policy |
| Application / OtaUpdateManager | custom manifest 验签、inactive slot write、内生 image identity、LocalRecordV2、最小本地 self-test、mark + readback、维护隔离、pair participant/tombstone | 私钥、跨两颗 Flash 原子承诺、依赖 host ACK 才确认本板 |
| 左板 Pair Coordinator | 两板 exact facts、right-first orchestration、pair intent/tombstone、数据面 release gate | 修改 Bootloader 语义、把请求 tx 当板端事实 |
| Linux updater | package/public-key verification、CH343 identity、逐命令 journal intent/fact、超时后只读 reconciliation、审计输出 | 在 ambiguity 下自动 Abort、通过 USB1/ROM 日常刷写、凭 generation alone 推进 confirmed |

---

# C. 最小核心代码修复

## C1. 修复分两层，禁止混成又一版大重构

### 第一层：止血 + 一次性裁决（第一版实际刷写候选）

只改：

- 清除 `app_main` 大型自动对象并临时提高 main stack；
- 加入安全 RTC/UART 首启证据；
- `validateLocalBoot` 记录 exact API error 和 post-state；
- 本地 validation 不依赖 pair/controller/USB；
- Activate 改成 intent-first；
- 不在 Confirm/timeout 决定镜像生死；
- host 严格 sequence/last_command；要求 local-validated flag。

不在这一版同时改 USB 数据面、stick/mouse、descriptor、桥接 FIFO 或其他无关逻辑。

### 第二层：在左板单板稳定后落地完整 V2 record/pair journal

- 左右板单 blob record、完成墓碑、image identity；
- pair record V2；
- updater per-board intent/fact journal；
- 全断电矩阵。

这两层可以处于同一 release branch，但第一轮实机先只打开左板 local path，right/pair 写操作保持禁止。

## C2. 左板 `app_main.cpp`

必须修改：

1. 把以下对象移到 file-scope `.bss`：
   - Secondary startup failure 分支的 `Frame request/response`；
   - 主循环 `kStickStatusCapacity` response buffer。
2. `sdkconfig.defaults` 暂时设置 `CONFIG_ESP_MAIN_TASK_STACK_SIZE=12288`。最终值必须以 `.su + uxTaskGetStackHighWaterMark(NULL)` 的最坏路径加至少 2 KiB 余量确定。
3. `nvs_flash_init` 精确记录返回值；**不要自动 erase NVS**，因为其中有 OTA/pair 证据。
4. local health 只包含：NVS/LocalRecord 可读、运行/boot partition/identity 一致、基本 allocator/RTC/UART0 控制面可用；不得包含 UART1、controller、USB1 枚举、pair health。
5. `otaPairHealth()` 要求右板 `kStatusLocalBootValidated`，不能只看 peer+health。
6. pair previous generation/slot 从真实 left/right STATUS 读取，不再都写 compile generation。
7. `savePairStage/clearPairStore` 失败必须使请求 NACK 或进入 MaintenanceRequired；当前 observer 吞返回值不可接受。
8. `recoveryAction()` 的 Continue/Confirm/Rollback 不能只计算；必须由一个幂等恢复执行器消费，或删掉伪状态机并让 host 明确接管。第一版建议保持 fail-close、只报告 action，不自动做多板写。

## C3. 左板 `OtaUpdateManager`

### `begin()` 调用顺序

```text
open NVS -> load newest valid RecordV2
-> get reset reason
-> get running partition
-> get boot partition
-> get running image state (exact err/state)
-> compute/read running image SHA
-> reconcile record vs running/boot/otadata
```

只有以下条件同时成立，pending image 才进入 `BOOT_PENDING` 并尝试 local validation：

```text
record.phase in {ACTIVATE_INTENT, BOOT_SELECTED, BOOT_PENDING}
record.tx != 0
record.target_generation == compiled_generation
record.target_subtype == running.subtype
record.image_sha256 == running_partition_sha256
image_state == PENDING_VERIFY or VALID
```

`VALID` 时应重建 `LOCAL_VALIDATED`，不是清记录回 Idle；是否释放数据面由 pair record 决定。

### `Prepare`

除当前 SHA/signature/project 检查外，必须：

- 精确记录每个 `esp_ota_begin/write/end/abort/get_partition_description` 返回值；
- 比较 descriptor 中的 generation/version 与 signed manifest generation；
- 保存 target slot、previous slot、image hash、manifest hash、peer range；
- PREPARED record 成功 commit 后才 ACK。

### `Activate`

正确顺序：

```text
persist ACTIVATE_INTENT(previous slot, target slot, tx, image hash)
-> esp_ota_set_boot_partition(target), capture ret
-> verify esp_ota_get_boot_partition()==target
-> persist BOOT_SELECTED + ret
-> ACK
-> bounded UART drain
-> restart
```

若 record 写失败：不调用 set_boot。  
若 set_boot 失败：把 exact error 写回 record，恢复 PREPARED/FAILED，不 ACK。  
若 set_boot 成功但后续 NVS 写失败：不伪造失败并自动 Abort；启动选择已改变，保持 intent evidence，重启后由 target/old app 重建。

### `validateLocalBoot()`

```text
if running identity mismatch -> FaultEvidence, do not mark
if image state == VALID -> local_validated=true, reconcile record
if image state != PENDING_VERIFY -> fail with exact state
if minimal local health false -> remain pending; no pair dependency
mark_ret = esp_ota_mark_app_valid_cancel_rollback()
post_ret/state = esp_ota_get_state_partition(running)
local_validated = mark_ret==OK && post_ret==OK && state==VALID
only then persist LOCAL_VALIDATED
```

mark 返回错误后不要再做早期 Flash 诊断；写 RTC/UART evidence，让 Bootloader 回退。旧槽启动后再把 RTC evidence 复制到 NVS 审计记录。

### `Confirm`

- 要求 state=LOCAL_VALIDATED/PAIR_PENDING，而不是在 Confirm 中调用 `validateLocalBoot()`。
- 先持久化 `PAIR_COMMIT_INTENT`，再写 completed tombstone；ACK 后才允许清理临时下载字段。
- 不删除最近完成 tx/gen/hash。

### `Abort/Rollback`

- 先写 `ROLLBACK_INTENT`，保存 exact previous slot。
- 当前槽仍 PENDING_VERIFY 时调用 `esp_ota_mark_app_invalid_rollback_and_reboot()`；若返回，记录 exact error，不清 evidence。
- 当前槽已 VALID 时可显式 invalidate 当前并 rollback，或选择 exact previous；无论哪种都必须验证 previous image 可启动，禁止用 `get_next_update_partition()` 猜 previous。
- 只有 old slot 已运行且状态可验证后写 `ROLLED_BACK` tombstone，再清 transient record。

## C4. 右板 `OtaReceiver`

1. 更新头文件过时注释：当前已有 P-256 verifier，不应继续写“build has no verifier”。
2. `persistState()` 改为返回 bool，使用单 `RightRecordV2` blob；所有 put/commit/clear 精确检查。
3. load 时：
   - Prepared 恢复为 Prepared；
   - Activated/BootSelected 需核对 running slot、running image hash/generation、image state；
   - Receiving 若无 chunk journal，恢复为 FailedEvidence 并要求从 0 重传；
   - 不得仅因 saved!=Idle 全部变 Failed。
4. Activate 同样 `persist intent -> set_boot -> verify boot -> persist fact -> ACK`。
5. `validateLocalBoot` 使用 minimal board health，不要求当前 controller 枚举；精确记录 mark/post-state。
6. response `sequence` 必须 echo 当前请求 sequence；不能回 `lastSequence_`。
7. Confirm 写 completed tombstone，不直接 `prefs_->clear()`。
8. Abort 不得先清 Preferences；保存 rollback intent，操作成功并在旧槽重建后再 tombstone。
9. 120 秒只适用于“尚未通过最小本地 health”的真正 pending image；一旦 `LOCAL_VALIDATED`，超时只保持 maintenance。

## C5. `makcm_ota_tool.py`

- `_read_response(expected_sequence, expected_command)` 必须同时过滤：tx、target、sequence；解 status 后要求 `last_command==expected_command`。
- STATUS 请求也要 exact sequence；迟到 ACK 不得被下一条请求接收。
- 所有 activation/confirm readiness 都要求 `STATUS_LOCAL_BOOT_VALIDATED`。
- `prepare()` 的通用 exception 不得直接 `safe_abort()`：先 exact STATUS；只有板端明确是同 tx 的 pre-activation Receiving/Prepared，才可显式 Abort。ActivationPending/PendingConfirm/LocalValidated 一律只读 reconcile。
- pair-prepare 的异常路径同理，不能对两个角色无条件 safe_abort。
- “already active/confirmed” 必须要求 board payload 中 `record_tx/completed_tx + image hash`，不能仅凭 generation/state/flags。

## C6. `update_proxy_pair.py` / journal

Journal schema 至少升级为：

```text
makcm.pair-auto-update.v2
new
right_prepare_intent -> right_prepared
left_prepare_intent  -> both_prepared
right_activate_intent -> right_local_validated
left_activate_intent  -> both_local_validated
pair_commit_intent
right_confirm_intent -> right_pair_committed
left_confirm_intent  -> confirmed
rollback_intent / rolled_back / maintenance_required
```

每次 state-changing command 前：原子 `save_journal(intent)`；每次响应或 STATUS 事实后：原子 `save_journal(fact)`。不能把整个 pair-activate 子进程包成一个未落盘黑盒。

每板 fact 至少保存：

```text
observed_at, response sequence/last_command,
record_tx/completed_tx, target/active generation,
image SHA, running/boot slot, image state,
local_validated, pair_committed,
reset reason, last ota op/esp_err, record sequence
```

恢复时以精确板端事实修正 journal；若只有 generation 一致但 tx/hash/tombstone 不明，保持原 phase，禁止晋升 confirmed。

`--controller-detached` 不应成为所有更新的永久前置条件。正确逻辑是固件执行 bounded quiesce；若 controller 在规定时间内无法安全 teardown，EnterMaintenance NACK、零写。验收矩阵必须同时跑 connected/disconnected。

## C7. 参考 unified diff

本报告同时提供 `MAKCM_OTA_MINIMAL_REPAIR_REFERENCE.diff`。它是基于附件快照生成的 **PATCHSET-1 审查参考补丁**，覆盖：

- main-task 临时栈余量与 `app_main` 大型自动对象止血；
- left mark-valid 的 pre/result/post 精确捕获、intent-first activation、禁止 Confirm/timeout 杀死本地已验证镜像；
- left 显式 rollback 不再先清证据，也不再用 `get_next_update_partition()` 猜 previous slot；
- right local-health 与 pair/data-plane health 分离、response sequence 回显、mark readback、禁止确认超时自动 invalidate；
- host strict `tx + target + sequence + last_command` 关联，并要求 local-validated flag。

该 diff **有意不实现** production V2 的单 blob A/B record、Prepared 重启恢复、完成墓碑、right intent-first Activate、Status V2 identity 和 updater per-board journal；这些必须按 C3–C6 与 B2–B4 作为第二独立 changeset 落地，不能混入第一次左板裁决刷写。

本审计只对冻结目录执行了 `git apply --check`，确认 unified diff 上下文可应用，并对其中修改后的 Python 工具做了语法解析；未运行 C/C++ 构建、单元测试或实机刷写。附件缺少最终 generated config、ELF/map 构建上下文及部分工程文件，因此 Codex 落地时仍须本地编译和审查，不得把 apply-check 当成功验收。

---

# D. 验收与 stop/go

完整逐项矩阵见 `MAKCM_OTA_ACCEPTANCE_MATRIX_CN.md`。本节给硬门槛。

## D1. 观测面必须在测试前就具备

每次 STATUS/审计必须能看到：

```text
role, protocol/status version
request sequence, last command
board record tx, completed tx
active/target/previous generation
running/boot/target/previous slot
signed image SHA + running image SHA
Bootloader image state + query esp_err
local phase, local_validated, pair phase, pair_committed
last reset reason, boot attempt
last esp_ota operation + exact esp_err
record sequence/CRC validity
```

缺任一关键身份字段，ACK-loss/旧 transaction/错 generation 测试不能判 PASS。

## D2. 硬 STOP 条件

出现任一项，停止刷写，不得创建下一 generation：

1. 新槽再次出现 ABORTED，但没有同一次启动的 P0–P8 证据或 exact mark/post-state。
2. mark 返回 OK，却没有 post-state=VALID。
3. post-state=VALID 后又观察到相同 slot ABORTED，且没有 raw otadata/bootloader hash。
4. package generation、ELF compiled generation、descriptor generation、image hash 任一不一致。
5. `app_main` frame/栈余量未测或低于 2 KiB。
6. 任一 `esp_ota_*`、NVS commit、pair record save/clear 返回值仍被吞。
7. ACTIVATE 仍为 set_boot-before-intent。
8. Confirm/120 秒超时仍能杀死已经 LOCAL_VALIDATED 的镜像。
9. STATUS 仍可在无板端记录时仅回显主机 tx，并被 updater 当作完成证据。
10. host 仍不校验 response sequence/last_command。
11. USB1 出现 CDC/DFU/vendor maintenance、或 USB callback 能到达 OTA handler。
12. controller/USB1/peer 被用于本地 mark-valid 门。
13. journal 只能表达全局 activated，无法指出哪块板哪条命令已 durable。
14. 断电测试后需人工擦 otadata/重新 BOOT 才能继续，而不能按 record/tombstone 确定恢复。

## D3. GO 门槛

### GO-0：允许构建第一版候选

- 附件外所需 generated artifacts 已冻结；
- D1/D3/D4 离线检查完成；
- 大型自动对象清除；
- exact evidence patch 代码审查完成；
- 无 early Flash diagnostic。

### GO-1：允许第一次左板单板 OTA

- 同一 candidate 的 build/package/ELF/map/config/hash 全部绑定；
- host sequence correlation 和 ambiguity policy 已修；
- 只走 Linux USB2，right/pair 命令禁用；
- 旧 gen5/当前 otadata/两槽 header 已只读备份；
- 有明确的物理 ROM rescue，但不预先擦写任何基础段。

### GO-2：允许右板单板和 pair OTA

左板 candidate 必须先通过：

- controller 不连接：50 次冷/软重启，local VALID、无 ABORTED；
- controller 连接：20 次维护 teardown/re-enumeration；
- USB1 连续拔插：不得出现维护接口，OTA 状态不变；
- local validated 后 host 失联 10 分钟，镜像不 rollback；
- ACK loss/duplicate/out-of-order/old tx 全部符合幂等矩阵；
- 每个关键断电点至少 3 次可确定恢复；
- 无需人工 BOOT/擦 otadata。

### GO-3：允许恢复日常自动更新

- 右板单板矩阵通过；
- 双板每个 pair 断电点通过；
- 两块板 completed tombstone 与 updater v2 journal 可互相对账；
- 两板混代时数据面始终 synthetic center/fail-close；
- final confirm 后 controller/XInput/HID/普通 mouse pass-through 回归通过；
- 生成一份完整、非模拟、单命令审计记录。

---

# 第一轮：只做什么，绝对不做什么

## 第一轮只做

1. **只读冻结**失败 gen14 的 package/bin/ELF/map/generated config/Bootloader/table/flash args，并计算 hash。
2. **离线裁决** `app_main` 真实 frame 与 main stack；核对 package generation、compiled generation、descriptor、image hash。
3. 只读 dump 当前左板 Bootloader、partition table、两个 otadata sector、两个 app header/hash；保留旧 gen5 可恢复证据。只输出脱敏 security/eFuse 状态，不读取 key block。
4. 落地最小止血/证据补丁：静态大对象、12 KiB 临时 main stack、RTC/UART probe、mark exact error+post-state、intent-first、host sequence correlation。
5. 对该候选做代码审查、静态分析和本地构建；明确记录“构建/测试结果属于 Codex 本地执行”，本报告不代替它。
6. 只有 GO-0/GO-1 全部满足后，执行**一次左板单板 OTA**；第一轮不更新右板、不做 pair Confirm。
7. 激活后 Linux 同一 CH343 会话切换到明确初始化的 115200 probe，抓取 P0–P10；若回退，任何人工复位前立即读取 gen5 status/RTC evidence/otadata。

## 第一轮绝对不做

- 不再用“gen15 换猜想、gen16 再换猜想”的方式试错；第一版候选若无裁决证据即停止。
- 不在 app_main 早期擦 coredump、checkpoint partition、otadata 或任意诊断 Flash。
- 不擦 NVS/otadata，不重刷 Bootloader/partition table，不调用日常 esptool/ROM 下载。
- 不改 mouse move/pass-through、stick 转换、USB descriptor/FIFO 等无关功能。
- 不更新右板，不进入双板 pair 激活，不恢复运行数据面。
- 不在 ACTIVATE/CONFIRM ACK 不明时自动 Abort。
- 不把 controller、USB1、UART1 peer 或 pair health 加回 local mark-valid 门。
- 不清除失败 record/tombstone 后再声称“状态已恢复”。
- 不索取、复制或嵌入生产私钥；继续使用既有离线签名流程。
- 不因 mock/unit/build 通过就宣布实机 OTA 修复完成。

---

# 附件源码审计要点索引

- 失败事实与时间线：`diagnostics/ota/GPT_PRO_OTA_FAILURE_DOSSIER.md:33-87`
- 左板分区/rollback defaults：`MAKCM_ESP32s3_Device_Mouse_Left_IDF/partitions.csv:4-9`；`sdkconfig.defaults:3-56`
- 左板恢复/mark/activate/record：`components/makcm_core/ota_update_manager.cpp:47-103,278-441,445-598`
- 左板大栈对象与启动顺序：`main/app_main.cpp:1317-1518`
- 左板 pair record/recovery：`main/app_main.cpp:669-775,966-1047`；`common/PairOtaTransaction.h:25-107`
- 右板持久化/恢复/activate/mark/abort：`src/ota_receiver.cpp:154-321,448-602,604-793`
- 右板普通/维护 UART 隔离与 health：`src/esp_tasks.cpp`
- 协议身份缺口：`common/OtaProtocol.h:76-111,143-173`
- manifest identity：`common/OtaManifest.h:19-37`
- host response correlation：`tools/makcm_ota_tool.py:958-1052`
- host prepare/activate/confirm ambiguity：`tools/makcm_ota_tool.py:1119-1313`
- pair CLI：`tools/makcm_ota_tool.py:1597-1824`
- updater journal/reconcile：`tools/update_proxy_pair.py:45-113,390-460,646-765`
- 测试盲区：`tests/test_right_ota_bootstrap.py`、`tests/test_makcm_ota_tool.py`、`tests/test_update_proxy_pair.py`、右板 Python 模型测试。

---

## 最终审计意见

现阶段最严谨的表述是：

> **左板连续回滚的 Bootloader 因果链已完全确定；原始早期复位的唯一触发器尚未由附件直接证明。当前源码中 `app_main` 大型自动对象/真实 main-task stack 是排序第一、且可在不刷板条件下一次裁决的候选。与此同时，mark-valid 的精确结果、运行镜像身份和最终 build/Bootloader 配置缺失，使 otadata API 失败、错包身份和配置漂移仍必须保留。**

修复工作的成功标准不是“某一代偶然启动”，而是：本板 `VALID` 事实独立成立、pair 只控制数据面、每个 state-changing 操作有 intent/fact/tombstone、主机能用 exact tx+image+slot+sequence 对账，并且所有断电点均能确定恢复而无需人工 BOOT。
