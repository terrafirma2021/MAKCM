# MAKCM 双板 OTA 交接

本文只描述当前发布路径。Windows 数据面连接左板 USB Device；可信维护 Linux
通过左板 CH343 执行 OTA。右板没有独立的主机侧更新入口。

## 固件与协议约束

- 左板：`MAKCM_ESP32s3_Device_Mouse_Left_IDF`，原生 ESP-IDF 5.5.4。
- 右板：`MAKCM_ESP32s3_HID_Mouse_Right`，原生 ESP-IDF 5.5.4 工程，使用组件管理器锁定 Arduino-ESP32 3.3.10。
- 两板共享 `common/OtaGeneration.h`，一次发布只使用一个 generation。
- 板端状态只有 `Idle`、`Receiving`、`Prepared`、`PendingConfirm`。
- 状态线上表示只有 `common/OtaStatusV2.h` 定义的 232-byte Status V2；主机必须严格解码完整结构。
- 普通命令的 ACK/NACK 只携带 `CommandResponsePayload` 定义的 24-byte
  命令结果，不是 Status 响应；`StatusResponse` 只使用 232-byte Status V2。
- `ExitMaintenance (0x0A)` 只释放 Right 的维护通道，不改变四态、事务、镜像或
  boot selection。主机仅在 Right 达到 `skipped`、`confirmed` 或 `updated` 终点后
  发送它；中间 Status 查询不退出维护。Left `status` 不发送终态命令。
- 左板只做目标路由。事务判定、重试和顺序由主机工具根据两板实时 Status V2 完成。

## 构建

在 ESP-IDF 5.5.4 环境中分别执行：

```powershell
idf.py -C MAKCM_ESP32s3_HID_Mouse_Right build
idf.py -C MAKCM_ESP32s3_Device_Mouse_Left_IDF build
```

发布签名只接受以下 application image：

```text
MAKCM_ESP32s3_HID_Mouse_Right/build/makcm_right_idf.bin
MAKCM_ESP32s3_Device_Mouse_Left_IDF/build/makcm_left_idf.bin
```

`MAKCM_ESP32s3_HID_Mouse_Right/main/idf_component.yml` 与
`dependencies.lock` 必须同时证明 IDF 5.5.4 和 Arduino-ESP32 3.3.10。

## 签名

生产私钥只保存在可信 Linux 签名机。工作站运行：

```powershell
python tools/sign_pair_release.py --host <signing-host> --user <user>
```

脚本从共享 generation 读取版本，先验证两个本地产物的内嵌 role、project 和 generation，
再上传 application image，在签名机生成并验签 `right-gen<N>.mkota` 与
`left-gen<N>.mkota`，最后拉回包、原始 image、SHA-256 清单和 release summary。

## 唯一部署入口

可信 Linux 主机只运行 `tools/update_proxy_pair.py`，串口参数必须指向左板 CH343。
命令需要左右签名包、生产公钥及其固定 SHA-256、signer key id、成功审计输出路径，
并必须显式给出 `--controller-detached`。该参数声明控制器已物理断开，是不可绕过的
写入前置条件，不改变协议判定。

```bash
python3 tools/update_proxy_pair.py \
  --port /dev/serial/by-id/<left-ch343> \
  --right-package diagnostics/ota/release_gen<N>/right-gen<N>.mkota \
  --left-package diagnostics/ota/release_gen<N>/left-gen<N>.mkota \
  --public-key /secure/path/production-p256.pub.pem \
  --public-key-sha256 <64-hex-sha256> \
  --signer-key-id 3dd2425f94e65ad13275fe6d1e2603bd \
  --audit diagnostics/ota/release_gen<N>/pair-update.success.json \
  --controller-detached
```

工具在写入前完整验签并核对两个包的 role、hardware、project、partition schema、
generation 和 peer generation。执行顺序固定为：

```text
Right: Receiving -> Prepared -> PendingConfirm -> Idle（健康且确认成功）
Left:  Receiving -> Prepared -> PendingConfirm -> Idle（健康且确认成功）
```

只有 Right 已完整回到健康 `Idle` 后才会开始 Left。任一步无法由当前 Status V2 精确
证明时立即失败，不启动后续板更新。成功后写出一次不可歧义的结果审计。

## 发布核对

1. 两板从同一提交、同一 generation 构建。
2. 右板构建日志与锁文件证明 ESP-IDF 5.5.4 / Arduino-ESP32 3.3.10。
3. `sign_pair_release.py` 的本地 identity 检查、远端验签和往返 SHA-256 检查全部通过。
4. `update_proxy_pair.py` 只连接左板入口，输出 Right `updated|confirmed|skipped` 后才开始 Left。
5. 最终两板 Status V2 均为目标 generation、`Idle`、本地启动有效且健康检查通过。
6. 保存 release summary、两个 `.mkota` 的 SHA-256 与 updater 成功审计。

无法用离线测试替代的证据包括：真实签名机私钥操作、CH343 身份、两板重启后的
Status V2，以及右板 USB Host 和左板 USB Device 数据面恢复。
