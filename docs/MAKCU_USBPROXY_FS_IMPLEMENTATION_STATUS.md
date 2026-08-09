# MAKCU USBProxy-FS 当前实现状态

## 当前架构

- 右板作为 USB Host 抓取下游设备描述符、配置和端点数据。
- 左板作为 USB Device 向主机复现已接受的 profile，并按端点维护有序传输。
- 板间数据使用带 framing 与完整性检查的 UART 协议；OTA 维护期间普通 USB 数据面停用。
- 未识别设备不会进入设备专用输入策略；profile capability 判定完成前不会启用上行 D+。
- `ProfileAccepted` 与 `StreamStarted` 是两个独立门，右板仅在左板确认 ready 后开放输入队列。

详细架构与硬件验收项见 `docs/PROXY_ARCHITECTURE_AND_ACCEPTANCE.md`。

## 构建基线

- 左板：原生 ESP-IDF 5.5.4，application 为
  `MAKCM_ESP32s3_Device_Mouse_Left_IDF/build/makcm_left_idf.bin`。
- 右板：原生 ESP-IDF 5.5.4，Arduino-ESP32 3.3.10 managed component，application 为
  `MAKCM_ESP32s3_HID_Mouse_Right/build/makcm_right_idf.bin`。
- 两板 application identity 和 release generation 都来自共享源码，不接受独立版本参数。

## 双板 OTA

- 维护主机只通过左板 CH343 连接；右板 OTA 帧由左板原样路由。
- 状态机只有 `Idle`、`Receiving`、`Prepared`、`PendingConfirm`。
- 唯一状态结构是严格 232-byte Status V2。
- `tools/sign_pair_release.py` 生成同 generation 的左右签名 application package。
- `tools/update_proxy_pair.py` 是唯一部署入口。它要求控制器已物理断开，先完整验签和
  核对两个包，再令 Right 从接收到确认后健康 `Idle`；只有此时才开始 Left，最终也必须
  回到健康 `Idle`。
- 任一实时板状态、包身份、启动状态或健康状态无法精确证明时停止，不推进另一块板。

操作步骤与发布证据清单见 `docs/OTA_HANDOFF_20260806.md`。

## 尚需实机证明

- 目标硬件上的右板 USB Host 枚举、热插拔与多端点持续流量。
- 左板在 Windows/Linux 的描述符、控制请求和每端点时序。
- 高速输入注入与物理设备流量并发时的长期稳定性。
- 真实签名包经单一 CH343 入口完成 Right 后 Left，并在两板重启后恢复数据面。

离线单元测试和两板构建是提交门槛，但不能替代以上实机证据。
