# GO-1 故障注入覆盖矩阵（断电 / 丢 ACK / NVS 写失败）

**范围**：为左板单板 GO-1 服务的最小自动覆盖。不扩展到双板 GO-2/GO-3。  
**原则**：能模型化的先模型化；不靠刷代数证明。

矩阵 ID 对齐 `MAKCM_OTA_ACCEPTANCE_MATRIX_CN.md`。

---

## 覆盖图例

- **AUTO**：仓库内已有可重复自动测试  
- **PARTIAL**：有相关测试，但不等价于矩阵原文  
- **GAP**：GO-1 前应补自动测试（模型/单测即可）  
- **LIVE**：仅实机；GO-1 单次受控时点检，不在本文件宣称已过

---

## A. 丢 ACK / 帧幂等（N-*）

| ID | 场景 | 覆盖 | 证据 |
|---|---|---|---|
| N-01 | 丢 MANIFEST ACK，重发同 tx/seq | PARTIAL | 右 `test_right_manifest_intent_exact_replay_no_second_ota_begin` |
| N-02 | 丢 DATA ACK，同 offset 重放 | PARTIAL | 右 `test_right_data_offset_seq_payload_replay_across_reboot` |
| N-03 | 同 offset 不同 payload | PARTIAL | 协议/工具测试拒绝路径（golden + tool） |
| N-05 | 丢 PREPARE ACK | PARTIAL | 右 `test_right_prepare_intent_fact_reconcile`；host resume prepared |
| N-06/N-07 | 丢 ACTIVATE ACK | AUTO | `test_update_proxy_pair.py::test_activated_ack_loss_*` |
| N-08 | 丢 CONFIRM ACK | PARTIAL | host tombstone / confirm recovery 测试（GO-1 不做 Confirm，降权） |
| N-09/N-10 | 迟到 ACK / last_command 错 | PARTIAL | host sequence 关联测试存在；未逐条贴 ID |
| N-12 | 重复状态变更命令幂等 | PARTIAL | 右 confirm/manifest replay |
| N-13–N-16 | WrongTransaction / 错身份 | AUTO | updater + tool + receiver 多项 |
| N-17 | 坏 CRC / 噪声 | AUTO | `test_ota_protocol` / tool decoder |

**GO-1 特有缺口**：左单板路径上的 N-05/N-06（Prepare/Activate ACK loss → 仅 STATUS 对账、不自动 Abort）缺少与右板同级的显式左模型用例 → **GAP-L-ACK**。

---

## B. 断电窗口（C-* 子集，左单板）

| ID | 注入点 | 覆盖 | 证据 |
|---|---|---|---|
| C-07 | target 首启、mark 前断电 | PARTIAL | 右 `test_right_local_valid_pre_mark_post_power_cuts_local_only`（局部）；左无对等模型 |
| mark 后、ACK 前断电 | GAP | — |
| PREPARED 后冷启动仍旧槽 | PARTIAL | host `resume_from_prepared`；板端事实依赖 Status V2 |
| LOCAL_VALIDATED 后 host 失联 10 min 不回滚 | LIVE/GAP | I-05/H-04；自动仅有“不超时 auto-abort”源码断言 |

**GO-1 最小应补（模型即可）** → **GAP-L-POWER**：

1. pending image 首启 + mark 前复位 → 下次 boot 允许 ABORTED + 旧 VALID；应用不得假装 VALID  
2. mark 成功写入 VALID 后复位 → 保持 VALID + local evidence，不因缺 Confirm 回滚  
3. PREPARED 未 Activate 断电 → boot 仍旧槽，record 仍可对账

---

## C. NVS / 持久化写失败

| 场景 | 覆盖 | 证据 |
|---|---|---|
| 右板 fact persist 失败 → FlashWriteFailed / fault | AUTO | `test_right_fact_persist_failure_forces_flash_nack` |
| 右板 snapshot torn write 同 seq 冲突 | AUTO | `test_right_snapshot_ab_torn_write_same_seq_conflict` |
| host journal A/B / 分叉 / 损坏 | AUTO | `test_update_proxy_pair.py` journal_* |
| 左 pair NVS A/B 不非原子双删 | PARTIAL | `test_pair_coordinator_v2.py` 静态约束 |
| 左 image-local / pair record 写失败 fail-close | GAP | **GAP-L-NVS** |

---

## D. GO-1 放行前最小自动补齐清单

已落地（模型，2026-08-06）：`tests/test_go1_left_fault_injection.py`（8/8 PASS）

1. **GAP-L-ACK**：左 Prepare/Activate ACK loss → 只读 STATUS 推进或 hold intent，禁止 safe_abort — **AUTO（模型）**
2. **GAP-L-POWER**：mark 前断电 → 旧 VALID + 新 ABORTED；mark 后断电 → 保持 VALID/local；PREPARED 未 Activate 仍旧槽 — **AUTO（模型）**
3. **GAP-L-NVS**：Prepare/mark persist 失败 → FAULT + fail-close，不声称 VALID — **AUTO（模型）**

仍属 **LIVE**（需实机，不在自动矩阵宣称已过）：真实断电、真实 NVS 介质失败、USB1 插拔。

现有右板与 host 覆盖 + 上述左模型对 GO-1 静态门禁为 **必要且已补最小自动集**；实机门禁仍看审核包 §0。

---

## E. 与实机 GO-1 的边界

自动矩阵通过 ≠ 允许刷机。还须：

- §G-08 当前 USB1 抓包  
- 签名 left-only 包与冻结 bin SHA 一致  
- 实机只读 otadata/bootloader/table 与构建产物对账  

任一项缺失：保持 **STOP**。
