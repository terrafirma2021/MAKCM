# MAKCU USBProxy-FS 修订方案

更新时间：2026-08-02

## 结论

MAKCU 保留“双 ESP32-S3 + 两条独立 USB 总线 + UART 桥”的硬件结构，但项目目标必须从“原始 USB 包透明转发”改为：

> 面向单个 USB Low/Full-Speed 下游设备、端点资源受限、以 Control/Interrupt 为主的通用主动 USB Proxy。

这是一种 Facedancer/USBProxy 风格的主动中间人：两边分别完成 USB 枚举，再转发控制请求和端点传输。它可以监控、修改和过滤数据，但不能保留原始 USB 线上每个 Token、DATA0/DATA1、ACK/NAK、CRC、SOF 和底层时序。

## 当前手柄的实测基线

手柄直接接电脑时，Windows 正常建立父设备并创建以下功能：

| 接口 | Windows 识别 | 端点 |
|---|---|---|
| IF0 | Xbox 360 Controller for Windows（XInput） | EP81 IN / EP01 OUT |
| IF1 | Xbox Security Method 3（该子接口单独安装失败，但不影响父设备） | 无 |
| IF2 | HID 键盘/消费者控制 | EP82 IN |
| IF3 | HID-compliant mouse | EP83 IN |
| IF4 | HID 游戏控制器/厂商定义 HID | EP84 IN / EP02 OUT |

因此鼠标是手柄自身声明的独立 HID 接口，不是 XInput 报告中的字段。该设备使用 4 个非控制 IN 和 2 个 OUT，正好接近两侧 ESP32-S3 的资源边界。

当前代理已经验证：修复“最后一个端点后提前结束接口描述符组”后，右板收到 `ProfileAccepted`，并成功转发过 15082 个输入报告；随后左板出现 `ESP_RST_PANIC`，所以当前优先级是先修运行期 panic，再扩展通用性。

## 必须修改的设计

### 1. Profile 改为原始 Blob + 能力表

右板读取并原样保存、分块发送：

- Device Descriptor
- 完整 Configuration Descriptor（按 `wTotalLength`）
- 所有 String Descriptor 和 LANGID
- BOS、Device Qualifier（设备提供时）
- HID Descriptor 与 HID Report Descriptor
- 其他未知/厂商描述符

左板不能因为“不认识某个 descriptor type”而截断、重排或重建配置 Blob。只有在能力规划阶段才解析副本；解析失败不应破坏原始 Blob。

需要扩展 `common/BridgeProtocol.h`、右板 `passthrough.cpp/esp_usb_host.cpp`、左板 `ProfileStore`，增加 Blob 类型、长度、CRC、能力字段和拒绝原因。

### 2. 在打开 D+ 之前完成能力规划

右板完成下游枚举后，左板先计算：

```text
FULL_PROXY
PROXY_WITH_ENDPOINT_REMAP
LIMITED_PROXY
UNSUPPORTED
```

至少检查：

- 是否 High-Speed-only
- 是否 Hub 或多配置设备
- 非控制端点总数是否超过 6
- IN 是否超过 4、端点号是否超过 6
- 是否包含 Isochronous
- 是否有复杂 Alternate Setting
- 估算 UART 带宽、队列和重传压力

不满足条件时，左板不拉起 D+，向 CH343 和右板返回明确的 `UNSUPPORTED_*` 原因，不能让 Windows 看到半套描述符。

当前版本保留 `PROXY_WITH_ENDPOINT_REMAP` 枚举值，但在没有完整的原始/虚拟
端点地址表和配置描述符重写器之前不宣称该能力；端点号超过 6 直接返回
`UNSUPPORTED_ENDPOINT_NUMBER`，避免把“可规划”误报成“可运行”。

### 3. EP0 采用本地状态机 + 选择性转发

本地处理：

- USB 标准设备状态、地址、配置状态
- 可安全缓存的 GET_DESCRIPTOR
- endpoint halt/status 和 reset 状态

转发并保留完整字段：

- `bmRequestType`
- `bRequest`
- `wValue`、`wIndex`、`wLength`
- DATA 阶段方向、实际长度、short packet、ZLP、STALL
- HID、XInput/vendor、验证相关请求

每笔 deferred control 必须有超时、request ID、取消和复位处理。控制请求不能与普通文本命令或输入帧共用无边界缓冲区。

### 4. 端点路由改成 class-agnostic

核心只依据端点能力工作：

```text
endpoint address + direction + transfer type + MPS + interval
```

不能把 EP83 固定写成“鼠标”、EP81 固定写成“XInput”。鼠标注入、手柄按键映射和过滤器必须位于独立的可插拔 policy 层；没有 policy 时，所有端点走原始转发。

每个端点需要独立的：

- FIFO/mailbox
- transfer phase
- DATA toggle/STALL/reset 状态
- UART sequence/CRC/重传统计

Interrupt 作为第一目标；Bulk 只在通过带宽预算后启用；Isochronous 第一版直接标记为不支持，不伪装成透明转发。

### 5. UART 协议保持通用二进制帧

统一使用：

```text
magic | version | type | sequence | session |
endpoint | flags | payload_length | payload | CRC
```

消息类型至少包括：

```text
PROFILE_BLOB_CHUNK
CAPABILITY_RESULT
CONTROL_SETUP
CONTROL_IN_RESULT
CONTROL_OUT_DATA
ENDPOINT_IN_DATA
ENDPOINT_OUT_DATA
STALL/HALT
BUS_RESET
SET_CONFIGURATION
SET_INTERFACE
SUSPEND/RESUME
DEVICE_GONE
STREAM_READY
```

右板不应再为鼠标、XInput、键盘分别编码不同的传输格式。

## 实施顺序

### P0：先稳定当前手柄

1. 刷入临时 UART panic 输出版，抓取左板 panic 回溯。
2. 修复 descriptor 分组后的运行期 panic。
3. 重新验证 Windows 父设备及 IF0/IF2/IF3/IF4 子设备。
4. 验证持续输入、手柄 OUT、鼠标 HID 同时工作。

### P1：原始描述符模型

1. 增加 BOS/Qualifier/未知 descriptor Blob。
2. 增加 Blob CRC 和长度覆盖测试。
3. 增加“D+ 未打开前 profile 必须完整”的门闩。

### P2：能力规划器

1. 实现四级 capability result。
2. 为端点、速度、配置、Alternate Setting、Isochronous、带宽建立 host 单元测试。
3. 将拒绝原因显示到 `LEFT_STATUS`/`DEBUG_STATUS`。

### P3：通用控制和端点代理

1. 完善 EP0 超时、STALL、reset、取消。
2. 完善通用 endpoint FIFO 和 backpressure。
3. 将 mouse/XInput 逻辑移入 policy/filter 层。

### P4：验收

- 直连与代理的 Device/Configuration/String/HID/BOS 对比
- Windows 父设备和所有可用子接口正常
- 4 IN/2 OUT 并行压力测试
- 连续热插拔、USB reset、PC 睡眠/唤醒
- 无 panic、无 UART CRC 错误、无 endpoint drop

## 能力边界

当前硬件不应承诺：

- 任意 USB 设备、任意端点数量
- USB High-Speed-only 设备
- Hub/多下游设备透明复制
- 持续 Bulk/存储、网卡、摄像头、USB 音频
- Isochronous 的原始帧时序
- 原始 USB Token/CRC/ACK/NAK 级别捕获

如果目标升级为“任意速度、任意设备、Hub 级完全透明”，需要改变硬件为 USB Hub、FPGA/专用 USB Host-Device Bridge，或具备独立高速 USB 控制器和更大缓存的方案；仅靠当前双 ESP32-S3 软件代理无法达到该目标。
