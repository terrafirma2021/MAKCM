# MAKCM OTA 可直接执行的验收矩阵

**适用基线**：`MAKCM_GPT_PRO_OTA_AUDIT.zip`，HEAD `acf3d25d994b4fea8bde411ce24df69f3b4a3d7b` 的冻结白名单快照。  
**性质**：验收设计；本审计没有执行构建、测试或实机刷写。  
**安全边界**：日常更新只经 Linux USB2/CH343；Windows USB1 不提供 CDC/DFU/MSC/vendor maintenance；不读取或索取私钥；破坏性槽损坏测试只允许在可牺牲样机且通过前置门槛后进行。

---

## 1. 状态与观测契约

本文使用纠正后的逻辑状态：

```text
EMPTY / RECEIVING / PREPARED / ACTIVATE_INTENT / BOOT_SELECTED
PENDING_VERIFY / LOCAL_VALIDATED / PAIR_COMMIT_INTENT / PAIR_COMMITTED
ROLLBACK_INTENT / ROLLED_BACK / FAILED
```

每次状态查询至少必须返回并写入审计记录：

```text
board identity, firmware project/version/build identity,
record_tx, completed_tx, target_generation, active_generation,
target image SHA-256, completed image SHA-256,
running partition subtype, boot partition subtype,
otadata image state, reset reason,
last esp_ota operation + exact esp_err_t,
last image-state query error,
local_validated, pair_committed,
last request sequence, last command, last NACK reason,
record sequence + CRC validity
```

**统一失败规则**：只要无法证明 exact `transaction + generation + image SHA + slot + image state`，就不得推进 journal、不得释放数据面、不得自动 Abort；状态进入“需对账/FAILED”，保留证据。

---

## 2. 前置静态与构建门槛

| ID | 操作/故障注入 | 预期结果与状态 | 必须观测 | 失败判据 |
|---|---|---|---|---|
| G-00 | 校验冻结 ZIP | SHA-256 必须等于 `275D8F03E0365BB3A3C98AA670ABBD33A4E555BBDFBA2FB84F0D46C8FAD7E87F` | 审计日志记录绝对路径、大小、SHA | 任一字节不一致即停止 |
| G-01 | 冻结失败 gen14 与新候选所有构建产物 | package/bin/ELF/map/`.su`/generated sdkconfig/bootloader/table/flash args 均可追溯 | 每个文件 SHA、工具链版本、git HEAD/dirty diff | 缺失败 ELF/map/config 或产物身份不唯一即禁止刷写 |
| G-02 | 检查 rollback 配置 | generated bootloader 与 application 均启用 rollback；实际 Flash bootloader hash 与预期匹配 | `sdkconfig.h`、bootloader config、只读 dump hash | 只看 `sdkconfig.defaults`；两侧配置不一致；实际 bootloader 不明 |
| G-03 | 检查 `app_main` 栈 | 大 `Frame`/1 KiB buffer 不在自动栈；ELF 序言/`.su` 证明有至少 2 KiB 审计余量 | `objdump` SP 调整、`.su`、map、配置栈 | 仅凭源码估算；真实 frame+余量超过栈；没有 `.su`/ELF |
| G-04 | 检查 image identity | manifest generation/hash 与 app bin、ELF 编译 generation、app descriptor version/project、role/schema 完全一致 | 解包报告与 SHA 对照表 | 允许“旧 bin 重新包装为新 generation” |
| G-05 | 检查分区身份 | generated CSV、partition-table.bin、Flash 上 table 三者逐项一致且无重叠 | 偏移/大小/subtype/hash | 只核对 CSV；烧录偏移或表 hash 不明 |
| G-06 | 静态 grep OTA/NVS 返回值 | 所有 `esp_ota_*`、关键 `nvs_*`、Preferences 持久化返回值被处理并记录 | CI 规则与人工审查清单 | 存在 `(void)esp_ota_*`、未检查 clear/commit、错误被压成 bool |
| G-07 | 协议 golden vectors | C++ 与 Python 对 wire/status 的字节级编码一致；坏 CRC/保留位拒绝 | 测试向量文件与结果 | 任一端容忍旧/模糊布局；sequence/last_command 不参与关联 |
| G-08 | USB1 descriptor 静态快照 | 只有产品所需 HID/透传接口；无 CDC、DFU、MSC、vendor maintenance | descriptor dump/Windows Device Tree/USBView | USB1 出现任意维护/刷写接口或 OTA parser 可达 |
| G-09 | 安全探针审查 | RTC_NOINIT + polled UART；不写 NVS/otadata/coredump/任何 Flash | 调用图/符号 grep | 早期探针调用 erase/write、创建任务、动态分配或 buffered log |

---

## 3. 正常更新路径

| ID | 场景与步骤 | 左板预期 | 右板预期 | Host journal 预期 | 观测点 / 失败判据 |
|---|---|---|---|---|---|
| L-01 | 左板单板 qualification；controller 未连接；只 Prepare/Activate，不 pair commit | 新槽 `PENDING_VERIFY -> VALID`；record=`LOCAL_VALIDATED`；USB1/数据面 fail-close；旧槽保留可回退 | 不变，精确报告旧 generation/identity | `left_local_validated`，不得写 `both_committed` | P0–P10、pre/mark/post state；任何 ABORTED、自动 rollback、依赖 controller 即失败 |
| L-02 | 左板单板、controller 已连接 | 先 synthetic center/release、USB1 detach；本地验证与 controller 无关；未 commit 前不重连 | 不变 | 同 L-01，并记录 teardown | controller 存在导致 mark 延迟/失败，或维护中仍有输入 report 即失败 |
| L-03 | 左板单板更新后使用兼容旧右板完成 pair commit | 左 `LOCAL_VALIDATED -> PAIR_COMMITTED`，完成墓碑 exact | 旧右镜像必须精确证明在 manifest peer range 内并写 pair tombstone；不重刷 | `pair_commit_intent -> both_committed` | 仅凭 generation/health 放行、无 image/tx/tombstone 即失败 |
| R-01 | 右板单板 qualification，经左板 UART1 转发，controller 未连接 | 左保持旧 VALID，maintenance coordinator；USB1 fail-close | 新槽 `PENDING_VERIFY -> VALID`，record=`LOCAL_VALIDATED` | `right_local_validated` | 右 local health 不得依赖 USB host/client/controller；否则失败 |
| R-02 | 右板单板、controller 已连接 | 先确认右 USB Host teardown，再 UART1 binary exclusive | 本地验证与 controller 无关；未 commit 不恢复普通 bridge | 记录 teardown 与 right facts | HID/普通日志与 MKOT 混流、旧设备端点继续传输即失败 |
| R-03 | 右板新镜像与兼容旧左板 pair commit | 左旧镜像写 exact pair tombstone并控制数据面 | 右 `PAIR_COMMITTED` tombstone exact | `both_committed` | 必须证明两个 completed identities；任一缺失不放行 |
| P-01 | 双板正常更新，controller 未连接 | 在右 local-valid 后才 Activate 左；左 local-valid；最终左 tombstone | 右先 Prepare、先 Activate、先 local-valid、先 pair commit | 逐板 intent/fact：R-prep→L-prep→R-act→R-valid→L-act→L-valid→R-commit→L-commit | 顺序改变、一个 opaque subprocess 后才落 journal、或 generation-only 对账即失败 |
| P-02 | 双板正常更新，controller 已连接 | maintenance 前 release/detach；commit 后再恢复 USB1 | 右 USB Host 停止；commit 后重新枚举 controller | 同 P-01，加数据面事件 | commit 前任何鼠标/手柄输出、重连旧 descriptor/session 即失败 |
| P-03 | 双板更新完成后的 50 次软重启 | 每次 running=boot=target、state VALID、tombstone不变 | 同左 | journal 保持 `both_committed` | 任一 ABORTED/INVALID、tx/hash漂移、维护态意外丢失/释放 |
| P-04 | 双板更新完成后的 20 次冷启动 | 与 P-03 相同；reset reason 合理变化但事实不退化 | 同左 | 不重复写镜像，不生成新 tx | 冷启动要求主机在线才能保持 VALID 即失败 |

---

## 4. USB1 / USB2 / controller 隔离

| ID | 故障注入 | 预期 | 观测 | 失败判据 |
|---|---|---|---|---|
| I-01 | OTA `RECEIVING` 时拔 Windows USB1 | OTA/record/otadata 不变；Linux USB2 会话继续；数据面已 fail-close | 左板 status 前后 exact diff | USB1 事件改变 tx/phase、触发 reset/Abort 或中断 CH343 |
| I-02 | OTA `PREPARED` 时反复插拔 USB1 20 次 | 保持 PREPARED；boot 仍旧槽；无维护接口 | USB descriptor 抓取、board facts | 出现 CDC/DFU/vendor interface；状态被清除或 Activate |
| I-03 | `LOCAL_VALIDATED` 但未 pair commit 时插入 USB1 | 不暴露维护接口；数据面不释放；镜像保持 VALID | Windows USBView + board status | Windows 能发 OTA、USB1 report 释放、或触发 rollback |
| I-04 | 完成 pair commit 后插拔 USB1 50 次 | 仅正常产品接口；每次全新 session/descriptor；OTA事实不变 | USB trace、session counter | 旧 session 复用、维护 interface、tx/tombstone改变 |
| I-05 | Linux USB2/CH343 拔出，板已 LOCAL_VALIDATED | 镜像继续 VALID；maintenance/fail-close；不按超时 rollback | 10 分钟 status/重启后状态 | 120 秒后回退、pair 假确认、USB1 接管维护 |
| I-06 | 使用非匹配 CH343 serial/bus path | updater 拒绝，零板端副作用 | host audit + board pre/post status | 仅 VID/PID 匹配就操作，或创建新 tx |
| I-07 | controller 在 maintenance teardown 中卡住 | bounded 超时后拒绝 EnterMaintenance/保持旧镜像；不开始写 Flash | teardown event、无 PREPARED | 一边普通 HID 一边 OTA；强行写入后再猜状态 |
| I-08 | controller 在 update 中途接入 | 保持 maintenance；不启动普通 USB Host 数据流 | right/left gates | controller 事件影响 mark-valid 或破坏 UART1 ownership |

---

## 5. 帧、ACK、事务与幂等

| ID | 注入 | 预期响应/副作用 | Journal 预期 | 失败判据 |
|---|---|---|---|---|
| N-01 | 丢 `MANIFEST` ACK，重发同 tx/seq/payload | 返回同结果；不得二次 begin 或改变 target slot | 同一 pending action，收到 exact fact 后推进 | duplicate 创建新 handle、擦写不同槽、WrongTransaction |
| N-02 | 丢 DATA ACK，重发同 offset/seq/CRC/payload | exact replay ACK；next_offset 不重复增加 | 不推进 chunk 两次 | Flash 重写引起 offset 增加两次或 hash 双计 |
| N-03 | 同 offset/seq 但 payload/CRC 不同 | NACK `WrongOffset/BadTransaction`；零 Flash 副作用 | 保持原事实并记安全事件 | 当作 retransmit 接受 |
| N-04 | DATA 错序，发送未来 offset | NACK；next_offset 保持 | 不改变 phase | 自动填洞、跳过数据或 hash 继续 |
| N-05 | 丢 PREPARE ACK | STATUS 必须以 exact record tx/hash/slot 证明 PREPARED；重试幂等 | 从 `prepare_intent` 推进 `prepared` | 自动 Abort、重新覆盖另一槽、仅凭 state=3 推进 |
| N-06 | 丢 ACTIVATE ACK，旧应用尚未重启 | host 只读对账 intent+boot slot；若已选 target，不重复不明操作 | 保持 activate intent，取得 fact 后推进 | ACK 不明立即 Abort；无 boot slot 就猜“成功” |
| N-07 | 丢 ACTIVATE ACK，目标已启动/LOCAL_VALIDATED | exact tx/hash/running=boot/VALID 证明成功 | 推进 local_validated | active generation 相同但 tx/hash不明也推进 |
| N-08 | 丢 CONFIRM ACK | completed tombstone exact；重试返回同 tombstone | 推进对应 board committed | 清记录后空闲响应回显请求 tx并被当成功 |
| N-09 | 同一 tx 内迟到旧 ACK | host 因 sequence 或 `last_command` 不符而忽略 | 不推进 | 只按 tx/target 匹配 |
| N-10 | 响应 sequence 正确、`last_command` 错 | 忽略并继续等待 | 不推进 | 被当作当前命令 ACK |
| N-11 | response `frame.offset != status.next_offset` | host fail closed | 保留 pending action | 选择其中任一值继续写 |
| N-12 | 重复 state-changing 命令，同 tx/command/sequence/image | 返回原 durable fact；零额外副作用 | 幂等推进一次 | 重复 set_boot、清 tombstone、生成新 transaction |
| N-13 | 旧 transaction 对当前 active record 发命令 | NACK WrongTransaction/Busy；零副作用 | 记录拒绝事件 | 旧 tx 能 Abort/Confirm 当前事务 |
| N-14 | 同 tx、错 target generation | NACK Compatibility/WrongTransaction | 不推进 | 修改 record.target 或包身份 |
| N-15 | 同 tx、同 generation、不同 image SHA | NACK；必须标记协议冲突并保持原 record | 停止人工裁决 | 仅 generation 相同就接受 |
| N-16 | manifest role/project/schema/hardware 错 | 在 `esp_ota_begin` 前拒绝 | journal failed/prepared前 | 产生写 handle 或更改 boot slot |
| N-17 | frame CRC 错、截断、噪声前缀 | decoder fail closed；下一合法帧可重新同步 | 记录错误，不变 phase | 错帧触发命令、卡死普通/维护切换 |
| N-18 | sequence 回绕 `0xFFFFFFFF -> 0` | 按协议明确定义；建议 0 禁用并开启新 tx | 新 tx 或合法回绕事件 | 与旧缓存 sequence 冲突并误重放 |

---

## 6. 主机失联、超时与延迟

| ID | 注入点 | 板端预期 | Journal/恢复预期 | 失败判据 |
|---|---|---|---|---|
| H-01 | host 在右 PREPARED 后退出 | 右 PREPARED 持久化；左旧槽运行；数据面关闭 | 重启 updater 读取 exact right fact，从左 Prepare 继续 | Prepared 重启变 Failed 且事实丢失；自动 Abort |
| H-02 | host 在左 PREPARED 后退出 | 两板 PREPARED；boot slot 均仍旧槽 | 从 both_prepared 继续 | 任一板自行 Activate/清记录 |
| H-03 | host 在右 set_boot 后、ACK 前退出 | 右 record intent/boot selected；重启后 right local-valid 或 rollback fact可判定 | exact reconcile，不重写镜像 | 只看超时猜成功/失败 |
| H-04 | host 在右 LOCAL_VALIDATED 后退出 | 右永久 VALID、maintenance；左旧 VALID；数据面关闭 | 稍后从 left activate 继续 | 120 秒后右 rollback |
| H-05 | host 在左 set_boot 后、ACK 前退出 | 左按本地规则 VALID；右保持 LOCAL_VALIDATED | exact facts 后进入 both_local_validated | 主机离线导致左 ABORTED/显式 invalidate |
| H-06 | host 在两板 LOCAL_VALIDATED 后退出 | 两板保持 VALID；pair 未 commit，数据面关闭 | 从 pair_commit_intent 或前一 durable phase继续 | 自动释放数据面或自动 rollback |
| H-07 | pair 延迟 121 秒、10 分钟、1 小时 | 本地 VALID 单调不退；maintenance/fail-close | journal 保持 local_validated，允许 exact resume | 任何 confirm timeout 改写 image lifecycle |
| H-08 | host 在右 pair commit 后退出 | 右 completed tombstone；左 pair_commit_intent/local-valid | 重启后只执行左 commit | 重新 Confirm 右造成记录清除/tx变化 |
| H-09 | host 在左 pair commit 后、journal save 前退出 | 两板 completed tombstone exact；数据面可由板端规则决定或保持关闭到对账 | updater 以双 tombstone推进 both_committed | 因 journal 旧而重刷/rollback |
| H-10 | updater 被 SIGKILL 在“intent journal fsync 后、命令发送前” | 板端无变化 | 重启读取 pending_action，查询 exact facts后决定发送 | 无 intent 记录却发生命令；盲重试不同 package |
| H-11 | updater 被 SIGKILL 在“板 ACK 后、fact journal fsync 前” | 板端 durable fact存在 | exact reconcile 后推进 | 只按 generation/Idle 推进 |

---

## 7. 关键断电点矩阵

### 7.1 单板局部状态

| ID | 断电窗口（左右板分别执行） | 重启后确定性预期 | 观测点 | 失败判据 |
|---|---|---|---|---|
| C-01 | RECEIVING 任意 chunk 后 | 旧 VALID 槽运行；若无 chunk journal，事务明确要求从 Manifest/offset 0 重传；不得称 PREPARED | record CRC/phase、next_offset policy | 部分镜像被当 Prepared/Activate；静默清记录 |
| C-02 | image 完成、PREPARED record 写前 | 无 PREPARED 事实；旧槽/boot 旧；同 tx 可完整重做 Prepare | partition hash可丢弃、record absent | 仅因 slot 有字节就推进 |
| C-03 | PREPARED record 写后 | PREPARED exact tx/hash/target slot；boot=old | status V2/read-only partition identity | 重启映射 Failed 且无法重试 Activate |
| C-04 | ACTIVATE_INTENT 写后、set_boot 前 | running=boot=old；record intent；重试 set_boot | boot partition 与 record | 误报已激活、清 intent或自动 Abort |
| C-05 | set_boot 成功、BOOT_SELECTED fact/ACK 前 | running 可能 old，boot=target；由 intent+boot重建 BOOT_SELECTED | boot partition readback | 无法区分并猜测；重复选择错误槽 |
| C-06 | restart 请求写后、实际 reset 前 | 与 C-05 相同；下一次启动 target | record + boot | 旧 app 清 record后 reset |
| C-07 | target 首启、mark 前断电 | Bootloader 下一启动可将 target ABORTED并回旧 VALID；旧 app必须保留 exact FailedRollback 证据 | 两 otadata sector、reset reason、record | 把 ABORTED当原始原因；证据被 clear |
| C-08 | mark Flash 操作中断电 | 允许 otadata 双 sector恢复到可解释的旧/新有效副本；只能选择一个 VALID/可回退槽 | 两 sector raw+decoded | 两 sector解释不一致却继续自动更新 |
| C-09 | mark 返回 OK、post-state readback 前断电 | 下次以 otadata为权威；若 target VALID，补写 LOCAL_VALIDATED | image state、running/boot/hash | 因 record未更新而 rollback VALID镜像 |
| C-10 | post-state VALID、LOCAL_VALIDATED record 写前 | target继续 VALID；启动补写 local fact | otadata + image identity | local record比 otadata更权威并否认 VALID |
| C-11 | LOCAL_VALIDATED record 写后 | target VALID且状态单调；等待 pair，无超时 rollback | 10分钟/冷重启 | 状态退回 PendingVerify/ABORTED |
| C-12 | ROLLBACK_INTENT 写后、set previous/invalidate 前 | 保留 exact previous slot；重启继续 rollback | intent、boot/running | 清 evidence 或猜 next_update_partition为previous |
| C-13 | rollback操作后、ROLLED_BACK tombstone前 | old slot运行并验证 identity后补写 tombstone | old running/boot/state | 空闲但无法证明回滚哪一事务 |

### 7.2 Pair commit 断电点

| ID | 断电窗口 | 重启后预期 | 失败判据 |
|---|---|---|---|
| PC-01 | both local-valid 后、PAIR_COMMIT_INTENT 前 | 两板 VALID、数据面关闭；可重新发起 exact commit | 自动放行或 rollback |
| PC-02 | PAIR_COMMIT_INTENT 写后、右 commit 前 | 左 coordinator恢复 intent；先查询右 tombstone | 重建新 tx或重刷 |
| PC-03 | 右 tombstone 写后、ACK 前 | 右幂等证明 committed；左未 committed；继续左 | 右清 tx后只回显请求 tx |
| PC-04 | 右 ACK 后、left commit intent前 | 同 PC-03；journal可从 right_committed恢复 | 重新修改右镜像状态 |
| PC-05 | 左 tombstone写入中断电 | A/B record至少一个 CRC有效；重启选择最高有效 seq并补齐 | 单 key 半写导致全记录不可用 |
| PC-06 | 左 tombstone后、ACK/journal前 | 双 tombstone exact；对账推进 both_committed | 因 host journal旧而回滚/重刷 |
| PC-07 | both committed、数据面 release 前 | 重启以双 tombstone释放或按明确策略等待host；不得处于混代输出 | release不受 tombstone约束 |
| PC-08 | 数据面 release 中断电 | 下次先 synthetic center、新 session，再完整枚举 | 复用旧端点/session、产生粘键/摇杆非中心 |

每个 C/PC 点：模型测试至少一次；真实继电器/可控电源注入至少 3 次。C-08、双槽损坏等破坏性项目只能在专用样机、备份齐全且 GO-2 后执行。

---

## 8. 坏镜像、签名、安全启动与回滚

| ID | 场景 | 预期 | 失败判据 |
|---|---|---|---|
| E-01 | package SHA 与 journal pin 不符 | host 在开串口前拒绝 | 仍进入 maintenance/创建 tx |
| E-02 | manifest P-256 签名篡改 | board 在 `esp_ota_begin` 前 NACK Signature；旧槽不变 | 有 Flash write/boot变化 |
| E-03 | manifest key-id 非 trust anchor | 同 E-02 | 回退到其他 key或接受空 key |
| E-04 | image data一位篡改 | Prepare hash失败，`esp_ota_end`/record不进入 PREPARED；boot旧 | 仍 Activate |
| E-05 | app header/descriptor坏 | first chunk或 Prepare拒绝；旧槽运行 | 仅 project name检查而 generation错包通过 |
| E-06 | image内编译 generation与manifest不符 | Prepare拒绝 Compatibility/ImageIdentity | 启动后才发现并产生ABORTED |
| E-07 | role/project/hardware/schema错 | Manifest阶段拒绝，零写入 | 用同一签名包跨板写入 |
| E-08 | native Secure Boot 应用签名坏（仅实际启用时） | `esp_ota_end`或Bootloader拒绝；不得 LOCAL_VALIDATED；精确记录分支 | 自定义manifest签名通过就视为可启动 |
| E-09 | anti-rollback security version过旧（仅启用时） | Prepare/set_boot/Bootloader按实际配置拒绝；旧槽保留 | host以普通兼容错误吞掉安全原因 |
| E-10 | 显式 rollback：目标仍 PENDING_VERIFY | 先写 rollback intent；调用 invalidate/reboot；旧槽运行并写 tombstone | rollback API返回值忽略、先clear记录 |
| E-11 | 显式 rollback：目标已经 LOCAL_VALIDATED | 使用记录的 previous slot set_boot；不得用 next-update猜 previous；旧槽验证后 tombstone | 选择错误槽、把当前VALID标INVALID |
| E-12 | rollback ACK丢失 | old/target事实+tombstone幂等证明；host只对账 | 自动再次invalidate另一槽 |
| E-13 | 一个槽损坏 | Bootloader选择另一有效槽；应用记录失败槽并禁止自动重用直到重写验证 | 无限重启或释放数据面 |
| E-14 | 两个 app 槽损坏 | 进入Bootloader/ROM救砖边界；正常USB1仍不出现维护接口 | 运行时USB1暴露刷写；声称应用OTA可恢复 |
| E-15 | otadata两 sector CRC/seq异常 | 停止日常OTA，保留raw dump，按Bootloader实际选择进入人工恢复 | 擦otadata后再诊断 |
| E-16 | NVS record A损坏、B有效 | 选择B最高有效seq；状态可确定 | 单份损坏导致清空全部事实 |
| E-17 | A/B均损坏但otadata target VALID | 镜像生命以otadata为权威；数据面保持fail-close，要求人工对账 | 因NVS坏而invalidate有效镜像 |

---

## 9. Journal 与板端事实对账

| ID | 初始不一致 | 预期对账动作 | 失败判据 |
|---|---|---|---|
| J-01 | journal=`new`，右板 exact PREPARED 同tx/hash | 推进 right_prepared，不重写右 | 仅state匹配但hash不匹配也推进 |
| J-02 | journal=`prepared`，右 exact LOCAL_VALIDATED，左旧 | 推进 right_local_validated，继续左 activate | 重做右镜像/Abort右 |
| J-03 | journal=`activated`，两板 exact LOCAL_VALIDATED | 推进 both_local_validated/commit intent | 需要 ACK 历史而拒绝 durable facts |
| J-04 | journal=`activated`，左 active generation相同但 record_tx/hash缺失 | 停止，不得 promoted | 现有 generation-only规则推进 |
| J-05 | journal=`confirmed`，板端只有 LOCAL_VALIDATED无 tombstone | journal降级/标冲突，数据面关闭 | journal压过板端并放行 |
| J-06 | journal=`prepared`，板端无 record且boot旧 | pending action需人工判定或安全重做prepare；不假定成功 | 直接activate |
| J-07 | journal=`right_committed`，右 tombstone exact、左未commit | 只执行左 commit | 重复confirm右并清墓碑 |
| J-08 | journal旧，双板 tombstone exact | 推进 both_committed，记录reconciliation来源 | 重刷或rollback |
| J-09 | journal tx 与板 record tx 不同 | 停止；输出双方完整事实，不发state-changing命令 | 选择“最新generation”覆盖 |
| J-10 | journal image SHA 与板 record SHA 不同 | 安全冲突；零副作用 | 同generation就忽略hash |
| J-11 | 板空闲且只回显查询tx，无completed tombstone | 不能证明Confirm；保持旧journal阶段 | 把request tx当board-held fact |
| J-12 | CH343物理身份改变 | 停止并要求重新授权设备身份 | 仅端口名相同继续 |
| J-13 | staged package在journal创建后变化 | hash pin失败，停止 | 使用新字节沿用旧tx |
| J-14 | journal JSON尾部损坏 | 原子文件/备份恢复；无法恢复则只读板端并人工裁决 | 创建新tx并覆盖证据 |

---

## 10. 长稳与普通功能回归

| ID | 测试 | 预期 | 失败判据 |
|---|---|---|---|
| Q-01 | 左板 local-valid 后连续运行 30 分钟，无 controller/host | 不 reset、不 rollback；probe P9；heap/stack waterline稳定 | 任意复位或栈水位越界 |
| Q-02 | 左板 50 次冷/软重启 | 始终 target VALID；无新ABORTED | 任一回旧槽且无显式rollback |
| Q-03 | 右板 50 次冷/软重启 | 同 Q-02 | 同上 |
| Q-04 | 双板完成后 1 小时普通 mouse/pass-through | 无协议混流、无OTA状态变化 | 普通帧能触发OTA parser |
| Q-05 | controller热插拔20轮 | 每轮新session、中心态、无粘键；OTA facts不变 | session复用/非中心输出 |
| Q-06 | CH343控制命令与高速4 Mbps切换 | OTA仅在明确115200/binary会话；切换确定 | 模糊波特率自动识别导致错命令 |
| Q-07 | UART1 5 Mbps压力 + maintenance切换 | 切换前排空，维护独占，无普通日志穿插 | MKOT CRC错/普通frame泄漏 |
| Q-08 | mock/unit/model全通过 | 仅作为GO必要条件，不代表实机成功 | 因绿测跳过真实断电/槽状态检查 |

---

## 11. STOP / GO 门槛

### STOP-0：现在立即生效

在以下证据齐全前，**禁止再刷下一 generation**：

- 失败 gen14 的 exact package/bin/ELF/map/generated config/bootloader/table/flash args 已冻结并有 SHA；
- `app_main` ELF 真实 frame 与配置 main stack 已裁决；
- package generation、compiled generation、descriptor 和 image SHA 已逐项核对；
- 当前板 Bootloader/table/otadata/app headers 已只读保存；
- 新候选 probe 不进行任何 Flash 诊断写；
- mark-valid 的 pre-state、exact return、post-state 已进入可读取证据结构；
- ACTIVATE/CONFIRM ambiguity 不会自动 Abort；host 校验 sequence+last_command。

任何一项缺失：STOP，不生成“再试一个猜想”的版本号。

### GO-1：只允许一次左板单板候选

必须同时满足：

1. G-00～G-09 全通过；
2. 候选 diff 仅含 OTA止血/探针/必要栈修复，无USB/HID/stick/bridge无关改动；
3. 旧 gen5、otadata raw、两个app header/hash与物理救砖路径已备份；
4. host明确执行 left-only，不更新右、不Confirm pair；
5. 同一CH343会话能抓 P0–P10；回退后不做人工reset前能读取旧槽状态和RTC evidence。

出现任一情况立即 STOP：

- 未看到 P0；
- pre-state不是 `PENDING_VERIFY`；
- running/boot/record target slot不一致；
- compiled/image identity不一致；
- mark返回非OK；
- post-state不是VALID；
- mark=OK且post=VALID后仍出现ABORTED；
- reset reason/otadata/record无法对账；
- 需要人工BOOT/擦NVS/擦otadata才能继续。

### GO-2：允许右板和pair fault-injection

左板必须先通过：

- L-01/L-02、Q-01/Q-02；
- I-01～I-05；
- N-01～N-18 的模型/协议测试；
- C-01～C-13 中非破坏性真实断电点，每点至少3次；
- host失联10分钟无rollback；
- 无人工BOOT、无擦otadata。

### GO-3：恢复日常双板自动更新

必须通过：

- R-01～R-03、P-01～P-04；
- PC-01～PC-08；
- J-01～J-14；
- USB1 descriptor与权限回归；
- controller连接/未连接两套回归；
- 一份从 package pin 到双 tombstone、journal fsync、数据面释放的完整审计记录。

---

## 12. 第一轮操作清单

### 第一轮只做

1. 只读冻结失败 gen14 与候选产物并算 SHA。
2. 离线裁决 `app_main` stack frame、最终 rollback/config/partition identity、package与编译 generation。
3. 只读保存 Bootloader、partition table、两份 otadata、两个 app header/hash；安全配置只记录脱敏状态，不读 key block。
4. 只落地：大型局部对象静态化、临时 12 KiB main stack、RTC/UART只读probe、mark exact error+post-state、Activate intent-first、host sequence/last-command严格关联。
5. 本地编译、静态检查、单元与协议测试；结果由本地执行者签入审计记录。
6. 达到 GO-1 后只做一次 left-only OTA；不更新右，不 pair Confirm。
7. 激活后抓 P0–P10；若回退，先保存旧槽status/RTC/otadata，再做任何人工动作。

### 第一轮绝对不做

- 不再逐 generation 替换猜想；无裁决证据不刷。
- 不在早期启动擦写 coredump、checkpoint、NVS、otadata 或其他 Flash。
- 不擦 NVS/otadata，不重刷 Bootloader/partition table。
- 不改 mouse/pass-through、stick、USB descriptor、UART FIFO 等无关功能。
- 不更新右板，不进入双板 activate/confirm。
- 不在 ACK 不明时自动 Abort。
- 不让 controller、USB1、UART1 peer 或 pair health参与本地 mark-valid。
- 不先清 record/tombstone再 rollback/confirm。
- 不索取、复制或嵌入生产私钥。
- 不因 mock/unit/build 通过宣称实机修复完成。
