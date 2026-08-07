# GO-1 只读审核包（MKOT v2 / generation 15）

**捕获时间**：2026-08-06（离线准备续跑；未刷机；未 Activate）  
**实机门禁**：**STOP（仅差口头放行）** — 静态/只读材料已齐；无人值守仍禁止 Prepare/Activate  
**拓扑**：USB1=Windows 透传出口（常插）；USB2=Linux CH343；USB3=右板 Host  
**GO-1 定义**：左板单板 Prepare/Activate，证明新槽首启活过 mark-valid；不动右板、不做 pair Confirm

参考矩阵：`diagnostics/ota/gpt_pro_audit/deliverables/MAKCM_OTA_ACCEPTANCE_MATRIX_CN.md`  
静态冻结目录：`diagnostics/ota/go1_freeze_20260806/`（见其中 `inventory.json`）  
只读实机抓取：`diagnostics/ota/go1_live_readonly/`

---

## 0. 总裁决（当前）

| 门槛 | 状态 | 说明 |
|---|---|---|
| G-00 冻结 ZIP | **PASS** | `MAKCM_GPT_PRO_OTA_AUDIT.zip` SHA-256 = `275D8F03…FAD7E87F`（与矩阵一致） |
| G-01 gen14 失败产物冻结 | **FAIL / 缺口** | 无 gen14 package/bin/ELF/map；仅有叙事 dossier + gen6 otadata + gen10–12 诊断残片 |
| G-02 rollback 配置 | **PASS（构建侧）** | generated `sdkconfig.h`：`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=1`；右 bootloader SHA 与信任锚一致 |
| G-03 `app_main` 栈 | **PASS** | ELF `entry a1, 240`；`.su` `app_main() 240 static`；main stack 12288 |
| G-04 image identity | **PASS（含签名包）** | 左 bin SHA `69792BD5…689B5E`；signed `left-gen15.mkota` package SHA `d0ffdbbc…989695`；inspect/verify 通过；generation=15 / role=left / project=`makcm_left_idf` / signer=`3dd2425f…03bd` |
| G-05 分区身份 | **PASS（构建侧）** | CSV ↔ `partition-table.bin` 已冻结；**Flash 上 table 只读 dump 仍缺** |
| G-06 OTA/NVS 返回值 | **PARTIAL** | 既有 bootstrap/coordinator 静态测试覆盖关键路径；未做完整 CI grep 签收表 |
| G-07 协议 golden | **PASS** | 既有 C++/Python 测试全绿记录；本轮 `tests.test_go1_left_fault_injection` 8/8 绿 |
| G-08 USB1 descriptor | **PASS（当前 Idle 透传面）** | USB1 常插 Windows。当前无 `4131`（手柄未在 USB3）属正常；已对现网 HID 人设抓包（含 `1532:007b` Razer），无 CDC/DFU/MSC。见 `go1_live_readonly/usb1_g08_census_20260806.json` |
| G-09 安全探针 | **PASS（源码意图）** | 危险早期 Flash 探针已删；coredump-to-UART/Flash 关闭；**实机确认仍要只读** |
| 故障注入矩阵自动覆盖 | **PASS（模型最小集）** | `tests/test_go1_left_fault_injection.py` 8/8；真实断电/NVS 介质仍为 LIVE |
| 实机只读 Status（CH343） | **PASS（V1） / PARTIAL（V2）** | Linux `/dev/ttyACM0`：Idle、`active_generation=5`、`target_generation=0`、非 maintenance_exclusive；V2 超时。见 `go1_live_readonly/left_status_20260805_230837.json` |
| 签名包注入 Prepare 脚本 | **PASS（门禁打开）** | `go1_left_prepare.py` `PACKAGE_SHA` 已注入真实摘要；**仍不得无人值守 Activate** |

**结论**：三项只读门禁已齐。GO-1 **仍 STOP 于口头放行**：无人值守**不得** Prepare/Activate。未碰 COM3 / write_flash / NVS / otadata。

---

## 0.1 三项门禁对照（放行 Prepare/Activate 前）

| 项 | 状态 | 产物 |
|---|---|---|
| 签名 left-gen15 包 | **齐** | `go1_freeze_20260806/left-gen15.mkota` + `.sha256.txt` |
| CH343 只读 status | **齐（V1）** | `go1_live_readonly/left_status_*.json`（V2 仍 PARTIAL） |
| G-08 USB1 抓包 | **齐** | `usb1_g08_census_20260806.json` + 各 `usb1_*_*.json`；USB1 本就常插，无需重插 |

---

## 1. generation 15 静态身份（已冻结）

来源：`diagnostics/ota/go1_freeze_20260806/inventory.json`

| 产物 | bytes | SHA-256 |
|---|---:|---|
| left_app_gen15.bin | 469008 | `69792BD58FC26FAA70E270266FD1CE23FF8691AEF896160DBEBBA9B0F7689B5E` |
| left-gen15.mkota | 469200 | `D0FFDBBC8E6EBB0EC845176DBFA5D142408EB09BC1BFA493D9D21DF466989695` |
| left_app_gen15.elf | 8053568 | `9A9CC93247130F1499C5330B85FBF7E9039EDDFC1F9C735A27ED950BE386EC2F` |
| left_app_gen15.map | 5446812 | `5B4D7D380263CF07DAC0CE371F6550BD4C23D38B3C76DE2C668FC2F469618D3E` |
| left_bootloader.bin | 22336 | `575EF2D88E5600799DED3F3FE9E15ECA063B1CDF1FB630E950178D573545BD2F` |
| right_firmware_gen15.bin | 482224 | `E6A12B8A1B296F73B2FA80D061A1469654B5904A232A9E432F35BA4D99CC5042` |
| left_partition-table.bin | 3072 | `7B0775212028D27583C95F6FDED9232211BA8A8397DBA5ED05689D04C8C322D8` |

签名核对（可信 Linux，私钥未进仓库）：

- image SHA == 冻结 left app SHA
- manifest：generation=15，role=left，project=`makcm_left_idf`，min_peer=5，max_peer=15
- `signer_key_id` = `3dd2425f94e65ad13275fe6d1e2603bd`
- `verify` → `signature_valid: true`
- 旧包 `980687fb…`（min_peer=4）已作废；`go1_left_prepare.py` 继续拒绝该摘要

核对事实：

- `common/OtaGeneration.h` → `MAKCM_OTA_PAIR_GENERATION 15`
- 左 app descriptor：`version=makcm-g15`，`project=makcm_left_idf`，magic `0xABCD5432`
- `app_main` 固定帧 **240 B**；`CONFIG_ESP_MAIN_TASK_STACK_SIZE=12288`
- 工作区 dirty；冻结以本目录字节为准

---

## 2. generation 14 失败证据冻结状态

### 已有（可保留）

| 文件 | 作用 |
|---|---|
| `GPT_PRO_OTA_FAILURE_DOSSIER.md` | 失败时间线与 otadata 语义（新槽 ABORTED / 旧槽 VALID） |
| `left_otadata_after_gen6_failure.bin` | 同类 ABORTED 形态的 Flash 残片（gen6 后） |
| `left-gen5-running.bin` | 回滚基线 app |
| GPT Pro 审计交付物 | 根因树、验收矩阵、最小修复参考 |

### 明确缺失（G-01 未过）

- gen14 signed package / left application bin / ELF / map / `.su`
- gen14 当次 Activate 前后 exact Status V2 / journal / package SHA
- gen14 当次 raw otadata 双 sector dump

**处理策略（不刷机）**：缺失项标 `MISSING`；不重编 gen14。GO-1 放行依赖 gen15 静态证据 + 只读基线 + 故障注入覆盖。

---

## 3. descriptor / profile capture

| 对象 | 现状 | GO-1 要求 |
|---|---|---|
| 手柄 4131:3519 hub/proxy snapshot（历史） | 有 | 数据面回归用；**不是** USB1 无维护接口的充分证明 |
| 左板 TinyUSB 构建开关 | CDC/MSC/DFU = off | 静态 PASS |
| **当前** Windows USB1 透传人设抓包 | **PASS** | USB1 常插；现网 HID（如 Razer）无 CDC/DFU/MSC；见 G-08 census |
| stick/profile 候选 | `diagnostics/ds02_4131/…` | OTA 主线不混做；GO-1 不依赖手柄 |

---

## 4. 故障注入矩阵覆盖（自动）

详表：`diagnostics/ota/GO1_FAULT_INJECTION_COVERAGE.md`

本轮重跑：`python -m unittest tests.test_go1_left_fault_injection -v` → **8/8 OK**。

---

## 5. GO-1 受控步骤（仅当 §0.1 三项齐 + 口头放行后）

1. 只读基线：经 Linux USB2/CH343 读左 Status（已有 V1；可再试 V2）  
2. Windows：对 USB1 做 descriptor snapshot（**当前缺口**）  
3. **单次** left Prepare → Activate（`go1_left_prepare.py` 仅 Prepare；Activate 另脚本/手控）  
4. 同 CH343 会话抓 mark pre/return/post；期望 `PENDING_VERIFY → VALID`  
5. 失败则 STOP：先保存 status/otadata/RTC，禁止人工 BOOT/擦 NVS/擦 otadata

**明确不做**：右板更新、pair Confirm、Windows USB1 任何 OTA、stick/plant、COM3 `write_flash`。

---

## 6. 需要用户醒来后做的动作

**不是刷 COM3，也不是重插 USB1。**  
USB1 本就常插 Windows 透传口；USB2=CH343 已做过只读 Status；G-08 已按现网人设抓完。

当前唯一阻塞：

1. **口头放行**后，才允许经 CH343 **单次** left Prepare→Activate  
2. 不动右板、不做 pair Confirm、绝不 COM3 `write_flash`

可选补强（非放行硬门槛）：再跑一次 Status V2（本次 V2 超时）。

---

## 7. STOP 条件（保持）

- 未看到 mark 证据链就猜成功  
- post-state 非 VALID，或 VALID 后出现 ABORTED  
- 需要 BOOT / 擦 NVS / 擦 otadata 才能继续  
- package / compiled / descriptor / image SHA 任一不一致  
- 任何“再刷一个 generation 试一下”的提议  
- 无人值守 Activate
