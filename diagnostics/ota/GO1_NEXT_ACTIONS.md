# GO-1 下一步操作卡（不刷 COM3）

**不会**、**不要**用 Windows 下载口（例如 COM3）刷 application。  
OTA 可信入口只有 **Linux USB2 → CH343 → 左板 UART0**。COM3 仅救砖/bootloader 迁移。

## 拓扑（勿搞反）

| 口 | 接法 | 作用 |
|---|---|---|
| **USB1** | 左板 Device → **Windows（常插）** | 透传/转换器出口；无 OTA |
| **USB2** | CH343 → **Linux 副机** | 唯一 OTA/维护入口 |
| **USB3** | 右板 Host → 手柄/鼠标 | 上游设备 |

G-08 抓的是 Windows 上 USB1 当前透传人设，**不是**让你再插一次 USB1。没有 `4131:3519` 只说明手柄此刻不在 USB3，不代表 USB1 没接。

## 当前状态（2026-08-06）

| 项 | 状态 |
|---|---|
| 签名 `left-gen15.mkota` | **已齐**（SHA `d0ffdbbc…989695`） |
| CH343 只读 Status V1 | **已齐**（active=gen5 / Idle） |
| G-08 USB1 抓包 | **已齐**（现网 HID 人设无 CDC/DFU/MSC） |
| Prepare/Activate | **STOP**（等你口头放行；禁止无人值守） |

## 你醒来后若要放行 GO-1

口头说允许后，才经 **CH343/USB2** 跑单次 left Prepare→Activate（绝不 COM3、不动右板、不做 pair Confirm）。

## 产物索引

| 文件 | 作用 |
|---|---|
| `GO1_READONLY_AUDIT_PACK.md` | 总裁决 |
| `go1_freeze_20260806/` | gen15 静态冻结 + signed mkota |
| `go1_live_readonly/usb1_g08_census_20260806.json` | G-08 普查结论 |
| `go1_left_readonly_status.py` | 只读 status |
| `go1_capture_usb1_descriptor.py` | USB1 抓包 |
| `go1_left_prepare.py` | Prepare（已注入 SHA；勿无人 Activate） |
