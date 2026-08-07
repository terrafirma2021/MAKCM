# GO-1 left-only 签名打包菜谱（不刷机、不碰 COM3）

私钥只在可信 Linux 的 0600 目录；本仓库与 Windows 工作区**不存放、不搜索私钥**。

生产 `signer_key_id`（公钥锚已嵌入固件）：

```text
3dd2425f94e65ad13275fe6d1e2603bd
```

冻结左板 app（工作区已拷贝）：

```text
diagnostics/ota/go1_freeze_20260806/left_app_gen15.bin
SHA-256 = 69792BD58FC26FAA70E270266FD1CE23FF8691AEF896160DBEBBA9B0F7689B5E
```

## 在可信 Linux 上执行

把冻结 bin 拷到 Linux 后：

```bash
python3 tools/makcm_ota_tool.py package \
  --image left_app_gen15.bin \
  --output left-gen15.mkota \
  --private-key /path/to/production_p256.pem \
  --signer-key-id 3dd2425f94e65ad13275fe6d1e2603bd \
  --role left \
  --generation 15 \
  --min-peer-generation 5 \
  --max-peer-generation 15

python3 tools/makcm_ota_tool.py inspect --package left-gen15.mkota
sha256sum left-gen15.mkota left_app_gen15.bin
```

核对必须全部成立：

1. package 内 image SHA == `69792BD5…689B5E`
2. manifest generation == 15，role == left，project == `makcm_left_idf`
3. `signer_key_id` == `3dd2425f…03bd`
4. 公钥 verify 通过

把 `left-gen15.mkota` 与 `sha256sum` 输出放回：

```text
diagnostics/ota/go1_freeze_20260806/left-gen15.mkota
diagnostics/ota/go1_freeze_20260806/left-gen15.mkota.sha256.txt
```

然后才能改 `go1_left_prepare.py` 的 `PACKAGE_SHA`（脚本在 SHA 未注入前会硬拒绝 Prepare）。

## 已完成记录（2026-08-06）

在可信 Linux（`192.168.137.2`）用 0600 生产私钥签名，**私钥未进仓库**：

| 字段 | 值 |
|---|---|
| package | `diagnostics/ota/go1_freeze_20260806/left-gen15.mkota` |
| package SHA-256 | `d0ffdbbc8e6ebb0ec845176dbfa5d142408eb09bc1bfa493d9d21df466989695` |
| image SHA-256 | `69792bd58fc26faa70e270266fd1ce23ff8691aef896160dbebba9b0f7689b5e`（= 冻结 bin） |
| generation / role / project | 15 / left / `makcm_left_idf` |
| min_peer / max_peer | 5 / 15 |
| signer_key_id | `3dd2425f94e65ad13275fe6d1e2603bd` |
| verify | `signature_valid: true` |

作废旧包 SHA `980687fb2f03cfd3d9485bd1a2ed8ea172064e0decb40bca8ea15ea733b9a25f`（min_peer=4）。

`go1_left_prepare.py` 的 `PACKAGE_SHA` 已注入上述真实摘要；**仍不得无人值守 Activate**。

## 明确不做

- 不在 Windows COM3/下载口刷写
- 不 pair、不更新右板
- 不把私钥拷进本仓库

