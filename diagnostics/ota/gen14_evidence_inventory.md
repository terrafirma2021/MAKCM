# generation 14 失败证据清单（冻结盘点）

**盘点日**：2026-08-06  
**目的**：满足验收矩阵 G-01 的可追溯性；**不重建、不重刷 gen14**。

---

## 结论

| 类别 | 状态 |
|---|---|
| 叙事 / 审计文档 | 有 |
| 同类 otadata 形态样本（gen6 后） | 有 |
| gen5 回滚基线 bin | 有 |
| gen10–12 误诊产物 | 有（标明不可当原始根因） |
| **gen14 exact package/bin/ELF/map/sdkconfig/flash_args** | **缺失** |
| **gen14 当次 journal / Status V2 / package SHA** | **缺失** |

因此 G-01 对“失败 gen14 全产物冻结”为 **未通过**。  
已确认的工程根因（`app_main` 栈、`restoreFromSnapshotV2` generation 卡死、completed 墓碑 generation 误判）不依赖找回 gen14 二进制即可成立；但审计上不得声称“gen14 失败可字节级复现”。

---

## 仓库内已有相关文件

路径均相对 `diagnostics/ota/`：

| 文件 | SHA-256（本机 2026-08-06） | 备注 |
|---|---|---|
| `MAKCM_GPT_PRO_OTA_AUDIT.zip` | `275D8F03E0365BB3A3C98AA670ABBD33A4E555BBDFBA2FB84F0D46C8FAD7E87F` | G-00 PASS |
| `left-gen5-running.bin` | `F0C779E366C78ECB69A36BBB309997E51015A718BA9C2E58E687E8A0A541DE33` | 回滚基线 |
| `left_otadata_after_gen6_failure.bin` | `24592DF254A0BBABABAF48D10E20B4CB8BCABF9BDB3FE87F78BF16403EA23844` | ABORTED/VALID 形态样本 |
| `left_gen10_boot_checkpoint.bin` | （未重算） | 不安全诊断方案残留 |
| `left_gen11_*.bin` / `*.elf` / `*.raw` | （未重算） | 自诱发 panic，非原始根因 |
| `left_gen12_*.bin` / `*.elf` | （未重算） | 不完整 coredump |
| `GPT_PRO_OTA_FAILURE_DOSSIER.md` | — | 事实：新槽 ABORTED、旧槽 VALID；gen14 Activate 后约 3s 回 gen5 |
| `gpt_pro_audit/deliverables/*` | — | 独立审计与验收矩阵 |

工作区搜索 `*gen14*` / `*g14*`：**无匹配构建产物**。

---

## 若用户侧仍有 gen14 原件

请原样放入 `diagnostics/ota/gen14_freeze/`（不要重编译），至少：

1. `left-gen14.mkota` + 声明的 package SHA  
2. `left` application `.bin` / `.elf` / `.map`  
3. 当次 `sdkconfig` 或 `sdkconfig.h`  
4. updater journal / audit JSON（Activate 失败那次）  
5. 若有：otadata raw、Status 输出

然后对每个文件计算 SHA-256，追加到本目录 `inventory.json`。

---

## 与 GO-1 的关系

- **不因 gen14 产物缺失而否定** gen15 栈/P0 修复的静态证据  
- **因 gen14 产物缺失**，不得把“历史失败已字节冻结”写成 PASS  
- GO-1 放行继续依赖：gen15 冻结 + 故障注入缺口关闭 + 实机只读基线 + 签名包；见 `GO1_READONLY_AUDIT_PACK.md`
