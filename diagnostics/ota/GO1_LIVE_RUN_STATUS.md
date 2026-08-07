# GO-1 gen16 升级记录（2026-08-06）

## 背景

核对确认：正确修法是**删掉** `begin()` 的一律 Fault 拦截块，不是加特例。
板子当前仍跑「特例版」gen15：`PendingConfirm + local_valid`，**Abort=rollback_unavailable，Enter=invalid_state**，无法直接 OTA。

## 已完成（软件）

- `OtaGeneration.h` → 16
- 左板重建：`makcm-g16`，image `c94f56cf…38273bee`
- 签名：`left-gen16.mkota` package `5354c996…3cf13770`（signature_valid）
- 含你删拦截块后的正确 `begin()` + 保留的 `validateLocalBoot` V1/V2 并列分支

## 阻塞与恢复

当前协议门：validated PendingConfirm 不能 Enter/Abort。

利用**板上仍是错误 begin** 的短暂窗口：
1. **断电再上电** → 错误 begin 把 VALID+PendingConfirm 打成 Fault
2. 脚本 Abort→Idle→Prepare→Activate gen16
3. 再主动断电，验证 gen16 仍 `active=16, local_boot_validated=true`（真正关闭 GO-1）

后台：`python diagnostics/ota/go1_gen16_wait_and_ota.py`
