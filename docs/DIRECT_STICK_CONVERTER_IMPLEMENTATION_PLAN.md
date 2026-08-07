# MAKCM Direct-Stick 转换器实施与验收规格

状态：实施基线；协议/状态/codec/compose-only 已部分实现，实机门禁尚未完成  
适用对象：MAKCM 双 ESP32-S3 通用 USB 代理、Linux SecondaryAim  
当前实现事实的权威来源：`docs/MAKCU_USBPROXY_FS_IMPLEMENTATION_STATUS.md`

## 1. 文档目的与判定原则

本文规定在不连接 XIM 的情况下，将 Secondary 的连续视角控制请求转换为
物理手柄右摇杆状态，并在左板发出原始 controller IN report 前叠加该状态。
本文同时规定协议、状态机、报告 codec、参数标定、故障安全、观测指标、
分阶段门禁和回退规则。

本文不是“已实现”或“已通过硬件验收”的声明。任何阶段只能使用该阶段实际
采集到的证据得出结论，不得用离线单测替代 USB/硬件证据，也不得用某一端点
成功替代全部端点或全部故障场景。

判定优先级如下：

1. `MAKCU_USBPROXY_FS_IMPLEMENTATION_STATUS.md` 中的当前实现与实测证据；
2. 当前源码实际行为；
3. 本文提出的目标设计；
4. 旧架构或历史计划只作背景，不用于推翻前两项。

### 1.1 2026-08-03 架构修订结论

本项目的主转换路径是 `SUPPORTED_OVERLAY`：物理 controller persona、descriptor、
endpoint 和非目标 report bytes 继续透传，只在已支持的 report-format adapter 上
修改右摇杆字段。它不是要求每台手柄生成 calibration 的虚拟手柄系统。

运行边界分为：

| 模式 | 行为 | 是否要求物理手柄 | 是否允许 stick mutation |
|---|---|---:|---:|
| `RAW_PROXY` | 当前通用 USB 透传和 mouse ledger | 是 | 否 |
| `SUPPORTED_OVERLAY` | 当前 persona 原样透传，在 compose 副本上叠加右摇杆 | 是 | 仅支持的 report 格式 |
| `CANONICAL_VIRTUAL` | 开发板自行生成固定 controller persona | 否 | 独立 encoder |

V1 的完成目标是前两种模式。`CANONICAL_VIRTUAL` 仅在未来明确要求“完全不接物理
controller”时单独立项；它不得改变或取代当前 generic ordered FIFO，也不得作为
本方案阶段 0–6 的前置条件。

校准边界同时修订为：

- report center/min/max/sign 属于协议格式常量或一次性 adapter 取证，不做逐台人工校准；
- 物理摇杆未经 mutation 时始终保留原始 bytes，不以估计中心重写；
- 单台硬件漂移只由 physical-override on/off hysteresis 容忍，V1 不做持续自动回正；
- 需要标定的是游戏/FOV/灵敏度/ADS 的完整 plant（死区、曲线、方向和时间动态），
  不是每只物理手柄；
- 新 report 格式的人工动作属于开发者一次性 adapter qualification，不是用户配置流程。

## 2. 当前权威基线

以下是新功能必须继承、不得暗中改变的当前事实：

- Generic 非鼠标 IN 端点各自使用独立有序 FIFO，并公平调度。
- 不存在 VID/PID 特定的 XInput latest-wins 调度路径。
- FIFO 满时丢弃新帧并计数；不得用新状态覆盖尚未完成的旧报告。
- 原始 Device/Configuration/String/HID/BOS/Qualifier Blob 保持权威。
- 输入报告不能隐式创建 endpoint；endpoint 只由 RAW driver 的成功 open
  路径配置。
- Controller 特定解释必须位于可选 policy/codec 层，不得进入 generic
  transport、endpoint 选择或调度规则。
- 当前鼠标注入继续使用独立的相对量 ledger；只有完整 USB completion 才退休
  已保留的相对量。
- `EndpointInData/EndpointIn` 已有独立 uint16 序列观测；stick 功能必须保留
  这些统计语义。
- 当前右板冷启动、三轮下游独立热拔插、EP81/EP83 并行输入和双 OUT 已有证据；
  USB bus reset、PC 睡眠/唤醒、EP82/EP84 持续真实流量仍不能宣称完成。

### 2.1 不允许出现的实现

- 不得把 controller endpoint 改成 latest-wins 来“顺便解决”转换延迟。
- 不得在 bridge RX 时就覆写 FIFO 内的原始报告。
- 不得根据 VID/PID 或“看起来像 XInput”直接猜写 offset。
- 不得修改 descriptor、endpoint 地址、`bInterval` 或报告长度来承载转换。
- 不得给未知 profile 附加鼠标 collection 或虚构 controller report。
- 不得把 XIM 的 `px/count`、延迟、gain 或现有 Smith pending 直接当作
  direct-stick plant。
- 不得用一个 USB poll 的摇杆脉冲作为正式 `km.move` 转换语义。
- 不得同时混用显式 `stick_rate`、显式 `stick_state` 和 legacy move estimator。
- 不得把逐台手柄中心/端点扫轴设为 `SUPPORTED_OVERLAY` 的正常启动条件。
- 不得用运行时估计中心重写未被 synthetic 接管的物理 report。

## 3. 目标架构：透传叠加为主

```text
Linux SecondaryAim (144 Hz control tick)
  +-- MouseRelative backend
  |     +-- existing XimReportScheduler
  |           +-- km.move(dx,dy)
  |                 +-- existing mouse ledger
  |
  +-- ControllerStick backend
        +-- backend-independent AimIntent (yaw/pitch mdeg/s)
              +-- game plant inverse (static LUT or validated dynamic model)
                    +-- stick_rate / stick_state command
              +-- CH343 4 Mbps
                    +-- left-board command/session state
                          +-- game-calibrated StickSynth
                                +-- report-format adapter/codec

physical controller
  +-- right-board USB Host
        +-- protocol-v5 framed UART
              +-- left-board per-endpoint ordered FIFO
                    +-- choose FIFO head in original order
                          +-- copy to compose scratch buffer
                                +-- optional stick overlay mutation
                                      +-- copy to inflight buffer
                                            +-- TinyUSB submit to host
```

这里的 `game-calibrated StickSynth` 指通过分类和交叉验证的完整 game plant 与控制参数；
controller report 编码由 report-format adapter 定义，不依赖单台手柄中心校准。

`RAW_PROXY` 与 `SUPPORTED_OVERLAY` 共用同一物理 endpoint FIFO。overlay 不是新的
controller queue，也没有 controller latest-wins。未来若实现 `CANONICAL_VIRTUAL`，
它必须使用独立、明确标识的 synthetic endpoint producer，不能借名改变 generic
proxy endpoint 的 FIFO 语义。

### 3.1 板级职责

右板继续只负责：

- 枚举和读取物理 USB 设备；
- 获取并传输原始 descriptors/profile；
- 按 endpoint 独立排队并公平发送原始 IN 数据；
- 转发 OUT、EP0、lifecycle 和错误；
- 不解释或修改右摇杆。

左板新增并负责：

- 接收并验证 Secondary 的 stick 命令；
- 将 injection session 绑定到当前 physical bridge session；
- 管理 mode、seq、TTL、lease、fault 和 profile；
- 在 `EndpointRouter::chooseReport()` 已复制 FIFO head 到 `compose` 后，
  只修改本次 compose 副本；
- 成功 completion 后才按现有逻辑弹出原始 FIFO head；
- 记录实际 synthetic、physical、merged 和 emission 时间。

Linux 副机负责：

- 检测、跟踪、PI/预测和 144 Hz 控制请求；
- 在 mouse backend 与 direct-stick backend 之间显式选择；
- 读取并选择 report-format adapter identity 与 game calibration profile；
- 在 backend 量化前生成 yaw/pitch 角速度语义的 `AimIntent`；
- 使用 direct-stick plant 做 ego/Smith 预测；
- 不再把 direct-stick 正式路径量化成 1000 Hz 整数 mouse count。

## 4. 不可破坏边界

### B1：Raw transport 边界

`OFF` 或未激活 stick overlay 时，所有 controller report 必须沿当前 ordered
FIFO 原样转发。对于同一输入捕获，除已有 transport 元数据外，输出 report
字节、顺序、长度和 completion 行为必须与基线一致。

### B2：FIFO 边界

Stick overlay 不得改变：

- `fifo_head/fifo_tail/fifo_count` 的推进条件；
- FIFO 满时“丢新帧”的当前策略；
- fair drain 规则；
- 失败或短 completion 时保留 FIFO head 的行为；
- session/epoch 的 stale completion 防护。

如果 ordered FIFO 下出现高水位、排队延迟或 command-to-emission 指标超标，
该窗口必须判为 transport/capacity 失败。允许的修复方向是优化 fair drain、
TinyUSB/任务调度、UART 预算或源 report cadence；不允许通过覆盖旧报告、合并
FIFO slot 或启用 latest-wins 来让指标“看起来通过”。

### B3：Compose-only 边界

Codec 只接收本次 `compose` 副本。原始 FIFO slot、bridge frame、capture buffer
和 profile blob 均保持不变。除右摇杆字段和该 profile 明确定义的完整性字段外，
其余字节必须逐字节相同。

唯一例外是显式 `CALIBRATION_SESSION`：为了自动切换 HIP/ADS，可额外修改 adapter
明确声明的 ADS trigger/button 字段及其完整性字段。该 mutation mask 只在校准
session 和独立 ADS lease 有效时开放，普通 `SUPPORTED_OVERLAY` 永远不能借此修改
其他按钮或 trigger。

### B4：Profile fail-closed 边界

只有完整匹配受支持 report-format adapter identity 的 codec 才能激活：

- VID/PID、`bcdDevice`；
- active configuration descriptor SHA-256；
- 目标 interface、endpoint address、Report ID、report length；
- HID report descriptor SHA-256（如存在）；
- codec format/version；
- 需要重算的 counter/checksum/CRC 规则。

任一项不匹配时必须：原样转发、synthetic 为中心、增加 `profile_mismatch`、
拒绝 mode activation。

Identity 的粒度是 report 协议/descriptor 版本，不是单台设备序列号。一个经过
回放和实机验证的 adapter 可以显式列出多个等价 descriptor identity；不得使用
VID/PID wildcard，也不得因同一型号换了一只实体手柄就要求重新扫轴。

### B5：Mouse 兼容边界

`MOUSE_RELATIVE` 模式下，`km.move` 保持现有相对 mouse count 语义和 ledger
不变量。Stick 功能不得删除、重命名或复用 mouse ledger 的内部状态。

### B6：命令源互斥边界

一次 injection session 只允许一个 active source：

- `STICK_STATE_Q15`；
- `STICK_RATE`；
- `MOVE_COMPAT`。

其他来源的命令必须拒绝并增加 `source_conflict`。切换来源必须先回中并重新
建立 injection session。

### B7：时间边界

TTL 从左板完整验证并接受命令的单调时钟时刻开始计算，不从 Linux 发送时刻
计算。过期命令、旧 seq、旧 injection session 或旧 physical session 均不得
刷新 TTL。

### B8：物理输入边界

普通 raw passthrough 不对物理 stick 做 deadzone、归一化、重编码或滤波。
只有 overlay 模式为判断占用和合并而解码物理右摇杆；未经修改时仍发送原始
物理字段。

Report-format adapter 中的中心是编码基准，不是要求把每份物理 report 校正到
该中心。physical override 使用保守的 on/off hysteresis；中心附近的硬件漂移不得
被写回 report，也不得触发持续自学习造成用户持杆时的中心漂移。

### B9：无新鲜物理报告时禁止伪造边界

不得仅为“回中”而无限重放最后一份完整 controller report，因为其中可能包含
陈旧按钮、序号、时间戳或设备私有状态。TTL 回中适用于下一份新鲜物理报告。
若 active overlay 期间 controller report 流停止，必须进入 stream-stall
故障流程；若无法在保护窗口内发出新鲜中心报告，则断开 upstream USB，而不是
继续向 host 暴露可能粘住的旧状态。

### B10：证据边界

- 串口 write 成功不等于板端接受，更不等于 USB completion。
- descriptor 相等不等于行为相等。
- XInput poll 成功不等于所有 IN endpoint 无丢帧。
- 板端计数干净不等于 Windows delivery gap 达标。
- 游戏速度曲线通过不等于热插拔、TTL 或物理接管通过。

### B11：完整游戏 plant 边界

Controller report center 与完整游戏 plant 是两个不同层次：

- report center 由 adapter 编码定义，例如 int16 的 0 或 uint8 的 128；
- game deadzone 由游戏、平台、灵敏度、response curve、HIP/ADS 等设置决定；
- `stick_state_q15` 可以故意发送 deadzone 内的值用于字段验证；
- `stick_rate` 和 `MOVE_COMPAT` 的持续非零请求必须通过已分类并验证的 game plant
  inverse 映射，不能只补一个 deadzone 偏置；
- 零请求、TTL、fault 和未武装状态必须保持 synthetic 精确为零。

不得把“自动跨过游戏死区”实现为每收到一个孤立 `move(1)` 就输出固定摇杆脉冲。
激活必须基于带时间戳的持续速度估计和迟滞。

### B12：HIP/ADS 自动切换与 fail-release 边界

正式 plant calibration 的 HIP/ADS 切换由 calibrator 和板端 calibration actuator
完成，不依赖用户按提示手动保持按钮。要求：

- adapter 明确声明 ADS 控件的 report field、encoding、hold/toggle 语义和 integrity；
- 切换前先成功发出 synthetic stick 中心并确认至少 2 份 report completion；
- ADS press/release 使用独立短 lease；Secondary 停止刷新、CRC/session 错误、abort、
  TTL、hotplug 或 fault 时必须自动 release；
- “ADS 字段已发出”只证明 USB actuation，仍须通过视觉 FOV/reticle 状态或游戏
  telemetry 确认游戏实际进入/退出 ADS；
- 无法自动注入或无法确认 mode 时，ADS trial 为 `INCONCLUSIVE/NOT_SUPPORTED`，
  人工按键只能用于探索，不能生成正式可重复 profile；
- direct-controller 标定不得借用 mouse RMB 造成游戏输入法切换；必须注入当前
  controller persona 的 ADS 控件。

若 release report 无法在保护窗口内成功 completion，必须触发 upstream detach，
不能让 host/game 保持可能粘住的 ADS 状态。

### B13：中心 ROI 与完整 plant 的观测边界

当前生产视频链路在 Windows Host 的 DXGI 原始纹理上先取中心 ROI，再编码给 Linux
Secondary。现配置为 `960x960 -> 640x640`，Host 默认值仍是 `640x640 -> 640x640`；
Secondary 收到的 `frame.width/height` 只是编码后的 ROI，不是完整游戏画面。现有
`GpuMotionEstimator` 还是单一全局平移 SAD，XIM calibrator 初始化搜索半径为 32 px，
实现硬上限为 64 px。由此规定：

- `LIVE_CENTER_ROI` 继续服务日常检测和闭环，不因 direct-stick 功能改变；
- 中心 ROI 可用于静止检查、方向 sanity、低速短脉冲、局部启动响应，以及准星/FOV
  状态确实落在 ROI 内时的 HIP/ADS mode verification；
- 中心 ROI 不能单独证明高摇杆区完整曲线、最大转速、长时间加速/惯性、360 度累计角、
  大位移对角线或全 FOV 几何；搜索结果触边、失去重叠或运动模糊只能拒绝 trial，不能
  被解释成游戏已经达到最大转速；
- 禁止把编码图宽 `640` 直接配上完整游戏 horizontal FOV 做 `px -> angle`。若使用局部
  ROI，至少先按 capture manifest 还原完整画面坐标：

```text
u_full = crop_left + (u_encoded + 0.5) * crop_width  / encoded_width  - 0.5
v_full = crop_top  + (v_encoded + 0.5) * crop_height / encoded_height - 0.5
```

  再使用完整画面的相机内参或 `H = K R K^-1` 旋转模型；简单 `px/FOV` 只允许作为
  已验证小角度近似。

正式阶段 4 新增互斥的 `CALIBRATION_WIDE` 观测模式：停止生产 ROI sender/AI Controller，
由同一个 Host DXGI pipeline 对完整游戏 client rect（或完整目标 monitor）做等比例缩放，
通过专用 AimAU/meta 端口发给 standalone calibrator；标定结束后恢复原中心 ROI。推荐
先采用单流切换，避免第二个 VPP/NVENC 改变游戏 FPS。可选替代是 Windows 本地带 QPC
时间戳的全帧录像，或可信游戏 telemetry。只有 `CALIBRATION_WIDE`、全帧录像或 telemetry
之一通过质量门禁，才能生成完整 `GamePlantProfile`；只有中心 ROI 时阶段 4 必须标为
`PARTIAL/INCONCLUSIVE`，不能发布 `stick_rate`。

每次 wide session 都必须生成并哈希 `CalibrationObservationManifest`：monitor/client
尺寸、source rect、encoded 尺寸、缩放/letterbox 规则、帧率、FOV/内参、Host/Secondary
版本、视频/meta 端口、时间戳来源和 session id。隐式拉伸、未知 rect 或错 manifest
一律拒绝拟合。

## 5. 单位与命令语义

### 5.1 内部统一坐标

- `+X`：向右转；
- `+Y`：向上抬头；
- normalized stick：`[-1, 1]`；
- Q15 stick：`[-32767, 32767]`；
- 正式 rate：`mdeg/s`，有符号 int32；
- 所有设备特定 Y 符号在 codec 内转换。

### 5.2 Mouse 模式

`km.move(1,0)` 仍等于 `+1` relative mouse count。

### 5.3 Stick state 模式

`stick_state_q15(x,y)` 表示一个绝对 synthetic normalized stick 状态。板端
sample-and-hold 到下一条有效 state、显式零或 TTL。

该模式只用于 codec bring-up、字段验证、标定和诊断；不承诺等角速度。

### 5.4 Stick rate 模式

`stick_rate(x_mdeg_s,y_mdeg_s)` 表示期望的相机 yaw/pitch 角速度。板端根据当前
HIP/ADS `GamePlantProfile` 的静态 inverse LUT 或已验证动态模型生成 normalized
stick，并在真实 controller report cadence 上保持。

没有有效 calibration hash 时不得接受 `stick_rate`，但仍可接受受限的
`stick_state_q15` 诊断模式。

### 5.5 Legacy move compatibility

Stick 模式中的 `km.move(dx,dy)` 是带接收时间戳的 virtual displacement
impulse，不是 raw stick LSB，也不是单个 USB poll 脉冲。

默认兼容估计器：

```text
window_ms = 28
virtual_full_scale_cps = 72 * 144 = 10368
estimated_cps = sum(move counts in window) / window_seconds
request_fraction = estimated_cps / virtual_full_scale_cps
activation hysteresis = on 0.03 / off 0.02
post-window low-pass tau = 10 ms
requested_camera_rate = compat_gain * filtered_request_fraction
stick = inverse_F(game_profile, requested_camera_rate)
```

因此一个孤立的 `move(1)`：

```text
estimated_cps = 1 / 0.028 = 35.714 counts/s
request_fraction = 35.714 / 10368 = 0.003445
request_fraction < 0.03, synthetic stick remains centered
```

兼容模式必须同时收到独立 lease；没有 move 不能同时表示“零”“低速稀疏”和
“链路已断”。`request_fraction` 不是 stick 值；通过激活门后必须先转换成期望相机
角速度，再使用阶段 4 的 game inverse LUT。没有有效 game calibration 时
`MOVE_COMPAT` 不得武装。正式生产路径使用显式 rate/state 和显式零。

### 5.6 三类中心与参数归属

| 参数 | 来源 | 生命周期 | 是否逐台人工设置 |
|---|---|---|---:|
| report center/range/sign | report-format adapter | adapter version | 否 |
| physical neutral tolerance | override on/off hysteresis | firmware config | 否 |
| full game plant | synthetic Q15 的 steady/duration/step/ramp 标定 | game/settings hash | 是，但不动实体摇杆 |

Game plant 使用板端 synthetic `stick_state_q15` 和视频/host 测量完整稳态曲线、
方向差异和时间动态，不依赖用户把物理摇杆沿直线推到端点。

## 6. Secondary 控制协议 V1

### 6.1 兼容策略

现有 `km.version()`、`km.echo(0)`、`km.buttons()`、`km.move()` 和按钮命令保持。
新增命令使用可读 ASCII 行，数据行附 CRC16-CCITT，避免在第一版同时引入全新
二进制解析器。144 Hz、每行低于 100 bytes 时远低于 4 Mbps 链路预算。

CRC 计算范围为行首到 `*` 前最后一个字符，不含 `*hhhh` 和换行。

### 6.2 控制面命令

```text
km.caps()
km.stick.begin(format_id,game_cal_id,source)
km.output_mode(off|mouse|stick)
km.stick.profile(hip|ads,calibration_hash)
km.stick.clear_fault()
km.stick.status()
km.stick.cal.begin(calibration_hash)
km.stick.cal.ads(down|up,lease_ms)
km.stick.cal.abort()
```

控制面命令必须返回单一、可解析 ACK/NACK：

```text
STICK_ACK(op,physical_session,injection_session,mode,format_id,cal_id)
STICK_NACK(op,reason,current_state)
```

模式切换 ACK 只能在状态机达到目标安全状态后发送，不能在刚解析命令时提前
回复。

`km.stick.cal.*` 是未来 calibration-only 控制面；capability 未启用时必须 NACK。
`cal.begin` 只在 exact adapter、`stick_state_q15`、中心屏障和有效 game profile
identity 下建立独立 calibration session。`cal.ads` 只有在 adapter 声明 ADS actuator
且前一状态 completion 已确认时接受；`cal.abort` 必须先中心、release ADS，再结束
session，ACK 只能在两个动作都成功 completion 后返回。

V1 的 `calibration_hash` 只选择固件内已编译并通过结构验证的
`GamePlantProfile`；不通过串口临时写入任意浮点表。生成新 game profile 后重新
构建左板是可接受的 bring-up 方案。后续若增加 NVS/chunk loader，必须另有长度、
CRC/SHA-256、版本、原子提交和掉电回退设计，不能改变当前 fail-closed 语义。

`km.move` 只有在当前 injection source 明确为 `MOVE_COMPAT`、独立 lease 有效且
game profile 已匹配时才进入 compat estimator；`MOUSE_RELATIVE` 模式继续执行原有
mouse ledger，两个语义不能由命令内容自行猜测切换。

### 6.3 数据面命令

```text
km.stick.state(inj_session,seq,x_q15,y_q15,ttl_ms)*CRC16
km.stick.rate(inj_session,seq,x_mdeg_s,y_mdeg_s,ttl_ms)*CRC16
km.stick.lease(inj_session,seq,ttl_ms)*CRC16
```

`seq` 为 uint32，使用半范围规则比较新旧；相同 seq 视为 duplicate，不再次
应用，不刷新 TTL。跳号可接受最新命令但增加 `seq_gap`，旧号增加 `stale_seq`
并拒绝。

### 6.4 命令验证顺序

必须严格按以下顺序：

1. 行长度、括号、字段数和数值范围；
2. CRC；
3. 当前 physical bridge session 是否有效；
4. injection session 是否匹配；
5. state 是否允许该命令；
6. source 是否匹配；
7. seq 是否为新；
8. profile/calibration hash 是否仍匹配；
9. TTL 范围；
10. 接受命令、更新时间和状态。

失败项只增加对应计数，不得部分更新 filter、seq 或 TTL。

### 6.5 数据 ACK 和遥测

144 Hz 数据面不逐包同步 ACK，避免串口阻塞。左板每 250 ms 或状态变化时发
紧凑 telemetry，`km.stick.status()` 可立即查询：

```text
STICK_STATUS(
  physical_session,injection_session,state,source,last_seq,
  command_age_ms,profile_short_id,cal_short_id,
  synthetic_x,synthetic_y,physical_x,physical_y,merged_x,merged_y,
  override,fault,counter_snapshot...
)
```

Linux 只有看到 `last_seq` 前进，才能把“串口已写”升级为“板端已接受”；USB
实际 emission 仍由 `reports_modified`、`last_emitted_*` 和 USB capture 证明。

## 7. 状态机

| 状态 | synthetic 行为 | 可接受事件 | 退出条件 |
|---|---|---|---|
| `RAW_SAFE` | 中心；raw 原样 | caps、begin、mouse | exact profile 匹配后进入 `STICK_ARMING` |
| `STICK_ARMING` | 中心 | profile/status | 连续 2 份新鲜 controller report 以 overlay=0 完成后 ACK，进入 `STICK_IDLE` |
| `STICK_IDLE` | 中心 | 当前 source 的零/非零新 seq | 非零有效命令进入 `STICK_ACTIVE` |
| `STICK_ACTIVE` | sample-and-hold，经 LUT/slew | 新命令、零、TTL、override | 零到 `STICK_IDLE`；物理占用到 `PHYSICAL_OVERRIDE`；过期到 `EXPIRED` |
| `PHYSICAL_OVERRIDE` | synthetic 强制中心，物理原样 | 新命令只记录但不恢复 | 物理低于 off 门限稳定 20 ms 后进入 `STICK_IDLE`，必须再收到新 seq 才激活 |
| `EXPIRED` | 下一新鲜 report 立即中心，不走 release slew | begin/新 session | 成功发出中心后到 `STICK_IDLE`；stream stall 到 `FAULT` |
| `FAULT` | 中心；raw 尽可能继续 | status、clear_fault、new session | 故障条件消失且显式重新 begin；严重 stream stall 触发 upstream detach |
| `HOTPLUG` | 清所有状态 | 新 physical profile | 新 session 完整验证后回 `RAW_SAFE` |

### 7.1 模式切换屏障

`mouse/off -> stick`：

1. 绑定当前 physical session 和 exact profile；
2. 清 compat window、filters、residual、seq、TTL；
3. 连续观察并完成至少 2 份新鲜报告，overlay 均为中心；
4. 返回 ACK；
5. 才接受非零数据命令。

`stick -> mouse/off`：

1. 拒绝新的非零 stick 命令；
2. 清 synthetic/filter/compat state；
3. 等待至少 1 份新鲜 controller report 以 overlay=0 成功 completion；
4. 返回 ACK；
5. mouse 模式恢复现有 `km.move` ledger 路径。

## 8. Controller report-format adapter/profile 与 codec

### 8.1 Profile 内容

```text
format_id / version
full descriptor SHA-256
VID / PID / bcdDevice
interface / endpoint / Report ID / report length
right-stick X/Y offset
field type: int16_le / uint8 / other
protocol center/min/max and qualification envelope
sign_x / sign_y
integrity fields and update rule
supported physical merge modes
known report variants
```

### 8.2 Codec 接口职责

```text
match(profile_identity) -> exact / reject
decodePhysical(report) -> normalized right stick or reject
encodeSynthetic(compose, merged_normalized) -> success / reject
updateIntegrity(compose) -> success / not-required / reject
validateUntouched(original, compose) -> target-field-only / reject
```

Codec 不拥有 endpoint queue，不决定调度，不推进 FIFO，也不访问 TinyUSB。

Profile 是 adapter 的可追溯实例，不是用户设备 calibration。若两个实体设备具有
相同受支持 identity 和 report grammar，它们必须复用同一 profile。只有 descriptor、
Report ID、长度、完整性规则或字段语义发生变化时才新增 format version。

### 8.3 首批目标

- Xbox 风格 `4131:3519`：在真实捕获确认 report layout 后实现；不得仅凭
  XInput API struct 假定 USB raw offset。
- DualSense Edge `054C:0DF2` USB Report ID `0x01`：候选右摇杆字段为
  `report[3]/report[4]`，必须通过一次性 adapter qualification 确认。

同一 VID/PID 的不同 `bcdDevice`、descriptor hash、Report ID 或长度均视为不同
format identity，除非回放和完整性测试证明它们可显式归入同一 adapter 版本。

新增 adapter 的人工取证采用“neutral + 右/上方向锚点 + 顺/逆时针圆周”连续动作；
通过稳健统计识别字段、轴、sign 和范围，不要求操作者画出严格单轴直线。该动作
只在开发新格式时执行一次，不能出现在最终用户的正常启动流程中。

### 8.4 Adapter 解析层级

按以下顺序选择 adapter：

1. 已验证的 proprietary adapter，例如 Xbox USB 或 DualSense USB report family；
2. descriptor-driven Generic HID adapter；
3. 不支持，保持 `RAW_PROXY`。

Generic HID 只有在 HID Report Descriptor 能唯一确定 application collection、
Report ID、完整字段 bit offset/width、logical min/max 和右摇杆 Usage 时才允许生成
codec。Rx/Ry、Z/Rz 等 Usage 冲突、多个候选右摇杆、跨 report 依赖或未知完整性字段
均必须拒绝，不能把“自动解析”退化成猜测。通过语义 parser 的 descriptor schema
可以复用 adapter；proprietary report 则继续使用显式 identity allow-list。

## 9. 数学模型

### 9.1 Raw 与 normalized

对 adapter 定义的每轴中心 `C`、负端点 `R-`、正端点 `R+`：

```text
raw >= C: u = (raw - C) / (R+ - C)
raw <  C: u = (raw - C) / (C - R-)

u >= 0: raw = round(C + u * (R+ - C))
u <  0: raw = round(C + u * (C - R-))
```

再应用 codec 的 X/Y 符号，使内部坐标一致。最终按 adapter 端点 clamp。qualification
capture 用于验证这些常量和允许的设备差异，不在运行时持续重估或回写物理中心。

名义 bring-up 示例：

- Xbox int16：`u=+0.35 -> +11468`，`u=-0.35 -> -11469`；
- DualSense uint8、中心 128：`u=+0.35 -> 172`，`u=-0.35 -> 83`。

这些只是 `stick_state` 编码示例，不是 `move(1)` 映射。

### 9.2 游戏 plant 不是单一 deadzone

对 mode `m`、方向扇区 `phi`、stick 半径 `r` 和持续时间 `t_hold`，真实游戏可能是：

```text
camera_angular_rate = F[m, phi, r, t_hold, previous_state]
```

其中可能同时包含：

- inner/outer deadzone 与 enter/exit hysteresis；
- 非线性 response curve 和最大转速 cap；
- `+X/-X/+Y/-Y` 不对称；
- radial/axial deadzone、对角线压缩和方向扇区差异；
- input smoothing、启动延迟、加速、惯性和回中制动；
- HIP/ADS、倍率、FOV、灵敏度、FPS 和 response preset 差异；
- aim-assist bubble 等目标相关、非平稳效应。

因此不得在测到 deadzone 后直接采用 `stick = deadzone + k*rate`。必须先执行动态
plant identification 并分类：

| 类别 | 判据 | 允许的正式模型 |
|---|---|---|
| `STATIC_MONOTONIC` | 稳态单调，hold-time 依赖和重复漂移均在门限内 | 分方向/扇区 monotonic inverse LUT |
| `DYNAMIC_REPEATABLE` | 存在可重复的时间加速/lag，动态模型残差达标 | time-bucket LUT 或 delay+time-constant 模型 |
| `NON_STATIONARY` | aim assist/场景/历史导致重复性或模型残差不达标 | 禁止开放 open-loop `stick_rate`；仅诊断 state 或实验闭环 |

执行优先级分开：V1.0 先支持在固定设置下通过 `STATIC_MONOTONIC` 门禁的完整非线性
曲线；V1.1 才启用 `DYNAMIC_REPEATABLE` evaluator。不能为了“一次支持所有游戏”
同时上线未经验证的静态和动态模型。某个游戏被分类为 dynamic/non-stationary 时，
不影响 RAW/mouse/`stick_state_q15`，但该 game profile 的正式 `stick_rate` 保持禁用。

`STATIC_MONOTONIC` 的基本模型为：

```text
omega_ss = F_ss[m,phi](r)
r = inverse_F_ss[m,phi](requested_rate)
```

`DYNAMIC_REPEATABLE` 第一版优先使用可审计的时间桶，而不是过拟合高阶模型：

```text
omega = F_dyn[m,phi](r, hold_bucket)
hold_bucket = 0-50 / 50-100 / 100-200 / 200-400 / 400-800 / 800+ ms
r = inverse_F_dyn[m,phi](requested_rate, hold_bucket)
```

若 step response 能稳定拟合，也可存 `pure_delay + rise/acceleration time constant`，
但必须与 time-bucket baseline A/B，不能只凭视觉选择模型。

动态 `hold_bucket` 属于一次非零激活 epoch：从 rate 越过 activation-on 开始，同向
幅度变化可继续计时；显式零、方向反转、扇区跨越、HIP/ADS 切换、TTL、physical
override、profile/session/hotplug/fault 均清 epoch。若这些转换的留出回放不能达到
动态误差门限，该 profile 必须降级为 `NON_STATIONARY`，不能在线猜测 previous state。

Epoch 的生效时刻以第一份非零 synthetic report 的成功 USB completion 为准，不以
Linux send 或板端 command accept 为准。之后按板端单调时钟推进，因为 host/game
会保持最近收到的 stick 状态；成功发出中心、成功发出 physical-priority report、
session 清理或 watchdog detach 才结束。未成功提交的 compose 不得提前改变动态
状态，stream stall 则走现有 fail-close/detach，不能继续外推 hold bucket。

所有模型要求：

- HIP/ADS 和可改变 plant 的设置完全独立；
- 最少独立保存 `+X/-X/+Y/-Y`，正式版扩展八方向并在相邻扇区插值；
- 重复点取中位数和分位数，isotonic regression 只约束稳态半径方向单调；
- 使用分段线性或 monotone PCHIP，禁止无界外推；
- 零响应区保存 enter/exit，超出标定范围饱和；
- calibration identity 包含游戏版本、平台、FOV、FPS cap、灵敏度、response preset、
  deadzone UI 设置、HIP/ADS/倍率和 aim-assist 状态；任一项改变即 hash 失效。

### 9.3 Backend-independent AimIntent

正式 direct-stick 路径不能继续把现有 `Controller::want_dx/want_dy` 按
`max_counts -> max_q15` 线性缩放。该值仍是为 mouse/XIM 调过的 virtual count，
当前 `DirectStickPublisher` 的线性映射只允许用于阶段 3 字段/安全诊断。

应在 backend 量化前形成：

```text
AimIntent {
  yaw_error_mdeg, pitch_error_mdeg,
  yaw_rate_mdeg_s, pitch_rate_mdeg_s,
  mode, timestamp, confidence
}
```

若可获得水平/垂直 FOV，像素误差先通过投影转换：

```text
fx = frame_width  / (2*tan(horizontal_fov/2))
fy = frame_height / (2*tan(vertical_fov/2))
yaw_error   = atan((aim_x-cross_x)/fx)
pitch_error = atan((aim_y-cross_y)/fy)
```

Outer visual controller 在角度域生成期望相机角速度；mouse backend 继续走现有
count 路径，stick backend 使用本游戏 plant inverse。若 FOV 不可靠，只能使用带
明确 identity 的 `mdeg_per_capture_pixel` 标定，不能继承 XIM `px/count`。

迁移必须先 shadow：同一帧同时记录 legacy `want_dx/dy` 和新 `AimIntent`，不改变
mouse live output。只有 direct-stick shadow 的限幅、方向、step/jerk 和闭环回放通过
后，stick backend 才消费 rate intent。

### 9.4 Rate 到 stick 的运行顺序

```text
AimIntent requested rate
  -> confidence/arm/TTL gate
  -> rate-domain low-pass and activation hysteresis
  -> select HIP/ADS + direction sector + hold bucket
  -> inverse plant model
  -> vector interpolation / circle clamp / saturation
  -> Q15 state
  -> board physical-priority merge
```

必须满足：

```text
requested_rate == 0  -> synthetic stick = 0
稳定 requested_rate != 0 -> model may cross measured game deadzone
```

由于硬死区存在，stick 数值从 0 跨到有效区可能是必然的；平滑应发生在角速度域、
激活迟滞和模型状态上，而不是使用 1-poll PWM。方向反转必须清同轴 hold bucket、
filter 和旧动态状态，并保持现有中心帧屏障。

### 9.5 滤波和 slew

先在 rate 域低通：

```text
alpha = 1 - exp(-dt / tau_rate)
filtered_rate += alpha * (requested_rate - filtered_rate)
```

再逆映射到 target stick，并在 normalized vector 域限制 slew：

```text
norm(target - previous) <= slew_per_second * dt
```

TTL、profile fault、session change 和 stream-stall 的回中不服从普通 release
slew。方向反转清同轴 compat window 和 filter，并至少经过一份 synthetic 中心
报告再进入反方向。

## 10. 物理与 synthetic 合并

### 10.1 第一版：physical-priority / neutral-only

- 物理幅度超过 `override_on` 并满足稳定门时，synthetic 立即为中心；
- 进入 latch 后不允许旧 synthetic 自动恢复；
- 物理幅度低于 `override_off` 并稳定 20 ms 后解除 latch；
- 解除后必须收到新 seq 才能重新进入 `STICK_ACTIVE`。

默认：

```text
override_on = 0.10
override_off = 0.06
override_stable_ms = 20
```

实际门限必须高于该手柄中心 jitter 的 p99.9 加安全裕量。

### 10.2 后续实验：additive-with-fade

只有 physical-priority 全部门禁通过后才允许实验：

```text
weight = clamp((T_high - |physical|) / (T_high - T_low), 0, 1)
merged = circle_clamp(physical + weight * synthetic)
```

Additive 不作为第一版默认；它会改变用户手感、饱和和 aim-assist 交互，必须
重新标定 merged plant，并单独验收松杆跳变和方向冲突。

## 11. 故障安全

### 11.1 Command TTL

默认 TTL 32 ms，允许范围 16–80 ms。更新频率 144 Hz 时约连续丢失 4–5 个
控制 tick 后触发。TTL 到期：

- 增加 `ttl_expiry`；
- synthetic/filter/compat state 立即清零；
- 下一份新鲜 controller report 中 synthetic 必须为中心；
- 成功 emission 增加 `reports_forced_center`；
- 不恢复过期命令。

### 11.2 Controller stream watchdog

运行时门限：

```text
stream_watchdog_ms = max(20 ms, 4 * measured_report_gap_p99)
bring-up hard cap = 100 ms
```

Active overlay 期间超过 watchdog：

1. 进入 `FAULT_STREAM_STALL` 并清 synthetic；
2. 等待一份新鲜报告；若到达，只允许先发送 overlay=0；
3. 若再经过 20 ms 仍无新鲜报告且没有 `DeviceGone`，断开 upstream USB；
4. 必须新 physical session 才能重新启用 stick。

Raw-safe 模式不因新增 stick watchdog 改变既有 transport 策略。

### 11.3 其他强制清状态事件

- physical session 改变；
- profile hash 改变；
- bus reset / SetConfiguration 重启；
- mode/source 切换；
- CRC 连续错误达到 fault 门限；
- codec mutation validation 失败；
- impossible raw range、错误 ID 或错误 report length；
- Secondary 端显式 disarm/zero。

## 12. SecondaryAim 补偿迁移

### 12.1 当前代码事实

当前 `Controller::update()` 输出的 `want_dx/want_dy` 是连续 virtual mouse counts；
其中 gain、max counts、方向 scale、EMA、PI、velocity FF、Smith pending 等均按
mouse/XIM plant 调整。当前 `DirectStickPublisher` 只是：

```text
virtual counts / max_counts * phase3_max_norm -> Q15
```

并且 direct 模式主动关闭 XIM calibration、ego、plant shadow 和 Smith。这一实现
适合作为阶段 3 codec/TTL/方向/回中诊断，不是可发布的游戏曲线转换器。不得通过
调一个 `max_norm` 就把它升级为正式 move-to-stick 路径。

### 12.2 可复用与必须隔离

可复用：

- Tracker、目标关联、预测和误差生成；
- 144 Hz Controller tick；
- PI、软启动、deadzone、方向反转清历史的结构；
- TTL、shadow/ego 框架和日志框架。

必须隔离到旧 mouse/XIM backend：

- `0.203442/0.124776 px/count`；
- XIM 约 `40.878 ms` delay；
- 现有 `gain=120`、`max_counts=72` 的已调物理含义；
- 1000 Hz fractional integerizer；
- `sent_dx * px_per_count` 和 count landed/pending 模型。

### 12.3 目标抽象

```text
Tracker / target prediction
  -> AngularIntentController
       angular error, requested yaw/pitch rate, confidence, timestamp
  -> ActuationRequest
       kind, requested_rate, mode, profile/session
  -> MouseRelativeOutput
       existing XimReportScheduler / km.move
  -> ControllerStickOutput
       game plant inverse -> stick_rate / stick_state

AppliedActuation
  requested rate
  board-accepted seq/time
  emitted synthetic Q15
  decoded physical Q15
  merged Q15
  emission time and hold interval
  override/TTL/saturation/fault
  calibration hash
```

实现时先新增 shadow 字段，不直接改 `ControlOutput::want_dx/want_dy` 的 live 语义：

```text
ControlOutput.legacy_want_counts   // 现有 mouse live path
ControlOutput.aim_intent_rate      // 新 direct-stick shadow/live path
```

迁移顺序：

1. 根据 FOV/投影把 target pixel error 转成角误差；
2. 新 `AngularIntentController` 在 shadow 模式生成 mdeg/s；
3. 记录 legacy count、rate intent、target error、mode 和 confidence；
4. 离线 replay 检查方向、饱和、step、jerk、反转和停止；
5. `stick_state_q15` 继续作为硬件诊断；
6. plant profile 通过后，direct backend 才切换到 `stick_rate`；
7. mouse backend 全程继续使用 legacy counts，除非另有独立回归项目。

Direct-stick ego/Smith 使用：

```text
predicted camera motion = integral(F[mode,dir,hold](emitted_stick(t-delay))) dt
```

不能继续使用 `dx * px_per_count`。若副机暂时没有 emitted telemetry，只能把
requested synthetic 作为估计，不能声称是实际 applied actuation。

### 12.4 闭环职责

Plant inverse 只提供 feed-forward 线性化，不能代替现有视觉闭环。运行时采用：

```text
外环：目标角误差/目标角速度 -> requested camera rate
前馈：game inverse model -> stick Q15
反馈：下一批视频帧的目标误差修正外环积分/比例
预测：使用 emitted stick + direct plant delay 的 Smith/ego（通过 shadow 后才启用）
```

Aim assist 打开时，bubble 内响应可能依赖目标、距离和历史，不能由空场静态曲线
完全描述。V1 calibration 应优先关闭 aim assist；若必须打开，则 plant 标记为
`NON_STATIONARY`，inverse 只作保守 feed-forward，正式结论依靠闭环 A/B，不能宣称
固定 rate 到 stick 的全局精确映射。

## 13. 观测指标定义

计数器必须单调递增，session 切换只通过 session ID 分段，不静默清历史。
状态输出同时提供当前 session 内增量快照。

### 13.1 Command counters

| 指标 | 精确定义 | 正常门限 |
|---|---|---|
| `cmd_received` | 完整收到的 stick 数据行 | 信息项 |
| `cmd_valid` | 通过全部验证并更新状态 | 确定性测试等于发送数 |
| `cmd_invalid` | 任一语法/范围失败 | 生产稳定窗增量 0 |
| `crc_error` | CRC 不匹配 | 增量 0 |
| `seq_gap` | 新 seq 跳过一个或多个值 | 确定性稳定窗增量 0 |
| `duplicate_seq` | seq 等于最后接受值 | 正常稳定窗增量 0 |
| `stale_seq` | seq 旧于最后接受值 | 增量 0 |
| `wrong_injection_session` | injection session 不匹配 | 增量 0 |
| `wrong_physical_session` | 命令绑定的 physical session 已失效 | 故障注入中必须增加且零应用 |
| `source_conflict` | 非当前 source 命令 | 增量 0 |
| `ttl_expiry` | accepted command age 达 TTL | 正常持续流 0；故障测试精确增加 |
| `lease_expiry` | MOVE_COMPAT lease 到期 | 正常兼容流 0；故障测试精确增加 |

### 13.2 Profile/codec counters

| 指标 | 精确定义 | 正常门限 |
|---|---|---|
| `profile_match` | 成功建立 exact format | 每 injection session 1 |
| `profile_mismatch` | 任一 identity 不匹配 | 支持设备 0；未知设备必须增加并零修改 |
| `codec_decode_error` | 物理右摇杆无法安全解码 | 增量 0 |
| `codec_encode_error` | 无法安全写入目标字段 | 增量 0，发生即 fault |
| `integrity_update_error` | counter/checksum 规则失败 | 增量 0，发生即 fault |
| `mutation_guard_error` | 非允许字节发生变化 | 增量 0，发生即 fault |
| `cal_ads_unsupported` | adapter 未声明可安全注入的 ADS field | calibration NACK，零修改 |
| `cal_ads_lease_expiry` | calibration ADS lease 未刷新 | 必须自动 release 并终止 trial |
| `cal_ads_release_failure` | release 未成功 completion | 必须 fault/detach |

### 13.3 Report counters and gauges

| 指标 | 精确定义 | 正常门限 |
|---|---|---|
| `controller_reports_seen` | 新鲜目标 endpoint report 入 FIFO | 信息项 |
| `reports_overlay_eligible` | profile/state/TTL 均允许修改 | 信息项 |
| `reports_modified` | synthetic 非中心且 compose 发生允许字段变化 | 与 active hold 时间相符 |
| `reports_forced_center` | TTL/fault/transition 导致 overlay=0 的 emission | 仅对应事件增加 |
| `reports_passthrough` | 未修改的原始 controller report | 信息项 |
| `physical_override_count` | 进入 override latch 次数 | A/B 场景可解释 |
| `physical_override_us` | override latch 累计时间 | A/B 场景可解释 |
| `saturation_count` | inverse plant evaluator 或 circle clamp 饱和 | bring-up 应接近 0；正式窗 <0.1% |
| `last_synthetic_xy` | 最近实际 emission synthetic Q15 | gauge |
| `last_physical_xy` | 同份 report 解码出的 physical Q15 | gauge |
| `last_merged_xy` | 同份 report 最终 Q15 | gauge |
| `last_emitted_seq` | 最近实际 emission 使用的 command seq | gauge |
| `last_command_age_ms` | emission 时命令年龄 | p99 < TTL |

### 13.4 Transport/USB 指标

继续使用现有指标并比较同一测试窗前后增量：

- controller FIFO high-water（按 endpoint 分开）、drop；
- bridge sequence gaps/duplicates/out-of-order；
- right-board overwrite/txerr/rxerr；
- left endpoint retry/drop/stale/short completion；
- USB stall/reset/re-enumeration；
- controller report interval p50/p95/p99/max；
- command-accepted 到 modified-report emission latency p50/p95/p99。

稳定窗硬门禁：drop、overwrite、CRC、sequence error、unexpected retry/stall、
unexpected re-enumeration 的增量均为 0。FIFO high-water 建议目标不超过深度的
25%；超过 50% 即使未 drop 也判为需调查，不能进入下一阶段。

### 13.5 Game plant 与控制指标

| 指标 | 精确定义 | 门限/用途 |
|---|---|---|
| `plant_profile_hash/class` | 当前 game profile 与分类 | 必须与 mode/settings identity 匹配 |
| `plant_trial_reject` | 因视频质量/时钟/状态错误拒绝的 trial | 信息项，不能混入拟合 |
| `cal_observation_source/hash` | ROI sanity / wide / full recording / telemetry 及 manifest | 完整 profile 禁止仅为 ROI sanity |
| `cal_motion_edge_hit` | 位移/金字塔层结果触及 estimator 可观测边界 | 正式 trial 必须为 0 |
| `cal_motion_inlier/coverage/residual` | 几何拟合质量与分区覆盖 | 内点率 `>=80%`，其余阈值由 observer qualification 固化 |
| `cal_clock_slope/offset/uncertainty` | Host↔Secondary↔board 仿射映射 | steady/dynamic 分别满足 4D.2 门限 |
| `cal_game_frametime_delta` | calibration capture 相对目标生产基线 | median `<=3%`，p99 `<=10%` |
| `plant_repeat_cv` | 同条件 measured rate 重复 CV | static `<=5%`；dynamic `<=8%` |
| `plant_model_cv_error` | 留出 trial 的预测/逆映射误差 | static 中段 `<=8%`；dynamic `<=10%` |
| `plant_hold_bucket` | 当前动态模型持续时间桶 | gauge，反转/零/TTL 时归零 |
| `plant_deadzone_enter/exit` | measured game activation hysteresis | profile 值，不是 controller center |
| `requested_rate_xy` | AngularIntent 或 compat curve 输出 | gauge |
| `compat_estimated_cps_xy` | MOVE_COMPAT 窗口估计的 count density | 仅 compat source 有效 |
| `compat_requested_rate_xy` | `H(cps)` 输出的 mdeg/s | 不得与显式 rate 混为同一来源 |
| `predicted_rate_xy` | emitted stick 经 plant forward model 的预测 | shadow/live 对齐 |
| `measured_rate_xy` | 视频估计的相机角速度 | 带 confidence/uncertainty |
| `intent_shadow_delta` | legacy count-path 与新 rate intent 的归一化差异 | 上线前 replay 审查 |
| `model_saturation` | requested rate 超出 profile valid range | 正式稳定窗 `<0.1%` |
| `cal_mode_verified` | visual/telemetry 确认 HIP/ADS 状态 | 每 trial 必须为真 |
| `cal_mode_verify_fail` | 字段已发出但游戏 mode 未确认 | trial 拒绝，不进入拟合 |

Plant 指标必须按 HIP/ADS、方向扇区和 profile hash 分组；不能把不同游戏设置的数据
汇总后得到一个看似稳定的平均曲线。

## 14. 通用验收判定

每个测试结果只能归类为：

- `PASS`：所有适用硬门禁和质量门均通过；
- `FAIL`：任一硬门禁失败；
- `INCONCLUSIVE`：缺少活动、缺状态窗口、采样工具受驱动占用、场景不稳定；
- `NOT_RUN`：未执行。

禁止把 `INCONCLUSIVE` 写成“基本通过”。

### 14.1 Go/No-Go 汇总

每个阶段的总判定为以下布尔条件的逻辑与：

```text
stage_pass =
  raw_regression_pass
  AND ordered_fifo_transport_pass
  AND protocol_safety_pass
  AND codec_integrity_pass
  AND fail_close_pass
  AND all_phase_specific_quality_gates_pass
```

| 子门禁 | 失败归属 | 不允许的替代证据 |
|---|---|---|
| Raw regression | descriptor/report/旧功能回归 | 新 stick 功能“手感正常”不能抵消 |
| Ordered FIFO transport | bridge/router/USB capacity | latest-wins、覆盖或 host poll 成功不能抵消 |
| Protocol safety | session/seq/CRC/TTL | 串口 write 成功不能抵消 |
| Codec integrity | profile/offset/sign/mutation mask | 游戏画面发生转动不能抵消 |
| Fail-close | TTL/stall/hotplug/mode transition | 正常连续运行不能抵消故障测试 |
| Plant quality | classification/curve/dynamics/delay | codec 字段正确不能抵消模型或速度误差 |
| Host delivery | OS/driver/application observation | 板端无 drop 不能抵消 host gap，反之亦然 |

任一适用子门禁为 FAIL，则阶段总结果为 FAIL；任一必要证据缺失，则总结果为
INCONCLUSIVE。只有全部适用子门禁为 PASS 才能进入下一阶段。

### 14.2 全阶段硬门禁

- Unknown profile 零 report 修改；
- `OFF/MOUSE_RELATIVE` 模式不改变 generic controller report；
- FIFO 顺序和 completion 退休语义不变；
- 所有故障最终 synthetic 回中或 upstream detach；
- 旧 session/seq 永不应用；
- 非目标字节 mutation 为 0；
- 不覆盖用户当前 mouse/XIM 路径。

## 15. 分阶段实施与测试

### 阶段 0：冻结旧行为与一次性 adapter qualification

#### 实施内容

- 固定当前 firmware、Secondary commit/文件 hash 和运行配置；
- 保存 mouse ledger 单测和 deterministic `km.move` trace；
- 保存 OFF/mouse 下目标 controller endpoint 原始 report trace；
- 对每个新增 report 格式选择一台代表设备，捕获 descriptors、endpoint、reports；
- 使用 neutral、右/上方向锚点、顺/逆时针圆周和其他控件排除实验；不要求严格
  单轴直线，也不把该动作交给最终用户重复执行；
- 建立可供同一 report identity 设备复用的 adapter/profile SHA-256；
- 记录原生 report cadence p50/p95/p99/max 和现有 FIFO/status 基线。

#### 必须产物

```text
diagnostics/stick/phase0/<format_id>/descriptor_manifest.json
diagnostics/stick/phase0/<format_id>/raw_reports.bin
diagnostics/stick/phase0/<format_id>/raw_reports.csv
diagnostics/stick/phase0/<format_id>/adapter_identification.json
diagnostics/stick/phase0/<format_id>/baseline_status_before.txt
diagnostics/stick/phase0/<format_id>/baseline_status_after.txt
```

#### 退出门禁

- exact endpoint、Report ID、length、X/Y offset、endian、sign、protocol center/range
  均有一次性工程取证或权威 descriptor/report grammar 证据；
- neutral 30–60 秒用于验证 adapter 容差和 override 阈值，不生成逐台运行 calibration；
- 至少两台相同 report identity 的实体设备应在不重新生成 profile 的情况下通过；
- OFF/mouse 基线下非鼠标 IN 仍为 ordered FIFO，窗口 transport 错误增量 0；
- direct 与 proxy descriptor/profile identity 可解释；
- 未确认 offset 的设备不得进入阶段 2。

### 阶段 1：协议和状态机，禁止修改报告

#### 实施内容

- caps、begin、mode/profile/status；
- injection/physical session 绑定；
- seq、TTL、lease 和 CRC；
- mode transition barrier 和 ACK/NACK；
- HOTPLUG、EXPIRED、FAULT、PHYSICAL_OVERRIDE 状态骨架；
- 全部 command/profile/state counters；
- mutation 总开关硬编码关闭。

#### 测试矩阵

- 正常 144 Hz 零/非零命令；
- 单包丢失、连续 1/3/5/10 包丢失；
- duplicate、乱序、uint32 wrap；
- 错 CRC、截断行、超长行、越界值；
- wrong injection session、旧 physical session；
- mode/source 冲突；
- controller hot-unplug/replug；
- Secondary kill、串口拔出、串口重连。

#### 退出门禁

- `reports_modified == 0`；
- 所有异常命令均零应用且对应计数精确增加；
- TTL/lease 后逻辑 synthetic 在 1 ms 内清零；
- hotplug 后旧命令永不恢复；
- 模式 ACK 只在 transition barrier 完成后出现；
- baseline controller reports 除测试本身外无任何字节变化；
- transport/USB 硬门禁增量 0。

### 阶段 2：Report codec 与离线 mutator

#### 实施内容

- 独立于 ESP/FreeRTOS 的 host-testable codec；
- int16_le 和 uint8 非对称量程编码；
- X/Y sign、radial/axis helper、circle clamp；
- exact profile matcher；
- integrity updater；
- allowed-byte mutation mask；
- physical-priority 状态逻辑的纯函数版本。

#### Host 单测

- 中心、正负端点、`-32768/32767` 非对称；
- uint8 127/128 中心差异；
- endian/sign；
- 超范围 clamp，无 wrap；
- axis/radial deadzone；
- 方向反转和中心帧；
- physical override on/off hysteresis；
- 错 ID、长度、hash、variant 拒绝；
- integrity 字段正确更新；
- 未允许字节 mutation guard。

#### 回放测试

- 至少 `10^7` 份合成/捕获报告；
- OFF 路径输出与输入逐字节一致；
- ON 路径只有右摇杆和明确完整性字段可变化；
- retry 同一 FIFO head 时原始 slot 不变；
- 不支持 profile 零修改；
- sanitizer/UBSan 可用时必须通过。

#### 退出门禁

- 所有 host tests 通过；
- `mutation_guard_error == 0`；
- `10^7` 回放无越界、wrap、错误长度或错误 ID；
- codec 尚不接入实际 USB emission，避免把离线正确误写成硬件通过。

### 阶段 3：只开放 `stick_state_q15`

#### 实施内容

- 在 `chooseReport()` compose 副本上接入 codec；
- 仅支持 exact profile；
- synthetic max 初始 0.20，通过后最多 0.35；
- 显式零、TTL、mode barrier、stream watchdog；
- last synthetic/physical/merged/emitted telemetry；
- 禁止 `stick_rate` 和 MOVE_COMPAT。

#### 硬件测试

- `0, +/-0.05, +/-0.10, +/-0.20` X/Y 单轴；
- 最大值逐步提升到 0.35；
- 四方向、对角线、反转；
- 每个状态保持和显式回中；
- USB capture 验证 report 顺序、长度和 cadence；
- kill Secondary、拔 CH343、命令停止、CRC storm；
- controller hotplug 三轮；
- bridge stream stall；
- OFF/mouse 与 stick 来回切换 100 次。

本节“单轴/对角线”指板端 synthetic Q15 测试向量，不要求人工沿实体摇杆画直线。
阶段 3 只验证 report 字段、方向、回中和安全性，不据此生成游戏 deadzone 参数。

#### 退出门禁

- Host/USB capture 的 raw 字段方向与 Q15 命令一致；
- 非目标字节变化 0；
- command accepted 到 modified emission：p95 不超过 1 个原生 report interval，
  p99 不超过 2 个；
- TTL 后 host 观察到 synthetic 中心不晚于 `TTL + 1 report interval`；
- 无新鲜 report 时按 stream-stall 规则 detach，不能无限粘杆；
- 100 次模式切换无旧 residual/state 恢复；
- 2 小时稳定运行无 unexpected stall/re-enumeration/drop/sequence error；
- 尚不评价游戏角速度精度。

### 阶段 4：Direct plant 标定与 `stick_rate`

#### 4A. 先实现独立标定 harness

标定不能运行完整 AI Controller，避免 PI/Smith/目标预测与 game plant 混在一起。
Linux Secondary 新增 standalone calibrator，逐 trial 发送确定性 `stick_state_q15`，并记录：

```text
trial_id / mode / direction / q15 radius / planned hold_ms
Linux send timestamp
board accepted seq/time
actual emitted Q15/time
USB capture timestamp
video frame timestamp / measured yaw-pitch angle and rate
```

只有 `actual emitted` 可作为 plant 输入；串口 write 或 requested Q15 不能替代。

Host 新增显式 `--capture-profile calibration-wide`（名称可在实现时调整），而不是让
操作者手写一组容易错配的 `--crop/--out`：

```text
production:
  DXGI full texture -> center ROI 960x960 -> 640x640 AimAU :5000/meta :5001

calibration-wide (exclusive):
  DXGI full texture -> full game client rect -> aspect-preserving wide encode
                    -> AimAU :5100/meta-v2 :5101 -> standalone plant calibrator
```

该选项必须强制 `wire=aimau`、禁止 stale-frame reuse、禁止 preview、声明 monitor/window、
校验 source/output aspect，并把 observation manifest short id 写入每帧 metadata。默认
建议从 `1280x720@120` 开始，但最终分辨率、帧率和码率由 observer qualification 与
实测 game frametime 决定，不把该建议写成普适常量。Secondary 已有 3840x2160 输入
上限参数，但 formal plant calibrator、wide geometry handshake 和专用启动脚本尚需实现。

若未来要同时输出 ROI 和 wide 两路，必须从一次 `AcquireNextFrame` 的同一原始纹理分支，
并证明相对单流生产基线的游戏 frame-time median 变化 `<=3%`、p99 变化 `<=10%`；否则
双流 trial 全部拒绝。V1 优先使用上述互斥单流，避免该干扰源。

Calibrator 同时包含 `CalibrationModeOrchestrator`，自动执行：

```text
SAFE_CENTER
  -> FORCE_ADS_RELEASE
  -> VERIFY_HIP
  -> HIP_TRIALS
  -> SAFE_CENTER
  -> ADS_PRESS_WITH_LEASE
  -> WAIT_ADS_SETTLE
  -> VERIFY_ADS
  -> ADS_TRIALS
  -> SAFE_CENTER
  -> ADS_RELEASE
  -> VERIFY_HIP
  -> COMPLETE
```

每次 mode transition 都记录 control command、板端 accept、目标字段 emission、视觉
mode confidence 和 settle time。`VERIFY_ADS/VERIFY_HIP` 优先使用视觉 FOV/reticle
classifier 或游戏 telemetry；仅看到 synthetic trigger 字段 completion 不足以开始
trial。任一步失败都进入 `SAFE_CENTER -> ADS_RELEASE -> ABORT`，release 失败则 detach。

#### 4B. 固定 calibration identity

- 游戏版本、平台、分辨率、horizontal/vertical FOV；
- 灵敏度、response curve preset、inner/outer deadzone UI 设置；
- FPS cap、VSync、controller smoothing/acceleration；
- HIP/ADS、倍率、武器/视角状态；
- ADS controller field/hold-or-toggle 语义、视觉确认规则和 settle-time envelope；
- aim assist、camera shake、recoil 等开关。

优先关闭 aim assist、acceleration、smoothing、camera shake 和 recoil。无法关闭的项
必须进入 identity；若导致重复性不达标，plant 分类为 `NON_STATIONARY`。

#### 4C. 自动激励计划

全部动作由 `stick_state_q15` 自动发送，实体物理摇杆保持松开：

1. 零输入基线：每 mode 30–60 秒；
2. coarse curve：`0,.02,.04,.06,.08,.10,.12,.15,.20,.30,.40,.55,.70,.85,1.00`；
3. deadzone：发现首次稳定运动后，在前后区间二分/0.002–0.005 细扫 enter/exit；
4. steady holds：每点 1.5–2.0 秒，回中 0.8–1.0 秒，末段估计稳态；
5. duration sweep：同一半径保持 `50,100,200,400,800,1500 ms`；
6. step：`0 -> .15/.25/.50/.85 -> 0`，每方向至少 30 次；
7. ramp/triangle：识别迟滞、时间加速、回中制动和路径依赖；
8. 方向：先 `+X/-X/+Y/-Y`，再四条对角线；必要时扩展 16 扇区；
9. HIP/ADS 和其他改变 plant 的 mode 全部分开执行，由 ModeOrchestrator 自动
   press/verify/release；操作者不参与逐 trial 切换。

垂直轴 trial 必须监测俯仰边界；进入游戏 pitch clamp 前停止并自动反向回到已验证
中间姿态。落入边界邻域的帧不得用于稳态拟合。若游戏的垂直可用行程不足以观测某个
duration bucket，则该 Y 轴动态项标为 `INCONCLUSIVE`，不能拿水平轴结果代替。

采用 coarse-to-fine 自适应采样：曲率大、deadzone 边缘、最大转速转折和模型残差大的
区间自动增加点，平坦区不机械堆积样本。

阶段 3 的 0.20/0.35 synthetic cap 不能直接跳到 1.0。Plant calibrator 使用显式
calibration session，依次开放 `0.35 -> 0.50 -> 0.70 -> 0.85 -> 1.00`；每一级先
确认方向、回中、USB 稳定性和视频未失锁，失败立即回中并停止。普通 live session
仍受发布上限约束，不能借 calibration 命令永久放宽。

#### 4D. 视频测量

##### 4D.1 观测源与几何

- 正式完整曲线使用 `CALIBRATION_WIDE`、Windows 全帧录像或游戏 telemetry；生产中心
  ROI 的现有 32 px global-SAD 只作为 `ROI_SANITY`，不得成为完整 profile 的唯一证据；
- 使用静态纹理、无目标和无动态物体场景；分区 mask 排除 HUD、枪模、准星、字幕和
  动态区域，并要求有效特征覆盖画面多个扇区，而不只集中在中心；
- wide 视频使用多尺度 rotation/homography estimator，按完整画面内参拟合 yaw/pitch，
  大位移不能长期使用简单 `px/FOV` 线性近似；
- observer 先用已知 FOV、已知旋转的合成序列做离线 qualification，再以静态场景的
  自动 360 度 loop closure 做端到端交叉检查；任何高速度区失锁都应提高采集帧率、
  改进多尺度估计或改用 telemetry，不能外推“最大速度”；
- 光流/特征内点率 `<80%`、估计落在搜索边界、有效覆盖不足、遮挡/运动模糊过多、
  几何 manifest 不匹配或帧间时间异常的 trial 直接丢弃并重测。

##### 4D.2 Host、Secondary 与板端三时钟对齐

当前 AimAU 已把 `frame_id + t_capture_host_us` 作为 H.264 unregistered SEI 绑定到
解码后的确切帧，这是正确基础；固定递增的编码 PTS 不是实采时间，禁止用于 plant
时间轴。正式校准升级为：

1. `DxgiGpuPipeline::Grab()` 输出 `DXGI_OUTDUPL_FRAME_INFO.LastPresentTime`，统一转换为
   绝对 QPC-us；只有驱动未提供时才使用 Acquire-return QPC，并把 timestamp source 与
   额外 uncertainty 写入 metadata。当前 `Grab()` 返回后调用 `HostSteadyUs()` 的时间戳
   只能作为近似，不足以发布动态 plant；
2. SEI v2 每帧携带 `cal_session_id, frame_id, t_present_host_qpc_us,
   observation_manifest_short_id, timestamp_source`。UDP meta 的 `FRAME` 副本只作诊断；
   frame 与时间的权威绑定是帧内 SEI，避免 UDP 乱序/丢包错配；
3. Host/Secondary 使用四时间戳交换：Secondary `t1` send、Host `t2` receive、Host
   `t3` send、Secondary `t4` receive。计算网络 delay，并对最低 RTT 样本做稳健线性
   回归，得到带漂移的 `t_secondary = a * t_host + b`，不能只用当前单 offset EMA；
4. calibration warm-up 至少收集 32 个有效 sync 样本，运行中持续刷新并输出 slope、
   offset、RTT、回归残差和 `sync_uncertainty_us`。稳态曲线要求 uncertainty
   `<= min(2 ms, 0.25 video frame)`；启动延迟/step 要求 `<= min(1 ms, 0.15 frame)`，
   不达标只允许稳态拟合，动态项标为 `INCONCLUSIVE`；
5. Secondary/板端另用带 `seq,t1,t2,t3,t4` 的短包同步板端 monotonic clock，拟合
   `t_secondary = c * t_board + d`。每个 trial 同时记录 requested seq、board accepted、
   actual report composition/completion 和 video frame id；串口到达时间不能冒充 emission；
6. 三域映射在 calibration session 内冻结版本并周期复核。时钟跳变、session 变化、
   sync 残差突增、SEI 缺失或 frame id 回退立即中心并拒绝当前 trial。

最终统一时间轴采用 Secondary monotonic-us；每个拟合样本同时保留原始三域时间戳、
映射参数和 uncertainty，保证离线可重算，而不是只保存换算后的单个时间。

#### 4E. Plant 分类与模型门禁

先拟合稳态 `F_ss(mode,sector,radius)`，再检查动态：

- `STATIC_MONOTONIC`：稳态 `R^2 >=0.98`，中段 CV `<=5%`，不同 hold duration
  的稳态中位差 `<=5%`，交叉验证逆映射误差中段 `<=8%`、临界区 `<=15%`；
- `DYNAMIC_REPEATABLE`：时间依赖可重复，time-bucket/delay 模型交叉验证误差
  `<=10%`，重复 CV `<=8%`，step onset/settle 残差有界；
- `NON_STATIONARY`：重复偏差 `>10%`、场景/目标依赖明显、方向或历史依赖无法由
  允许模型解释，或模型交叉验证失败。

方向误差要求中段 `<=3 deg`、deadzone 附近 `<=5 deg`。如果保持期间速度持续爬升，
绝不能仍输出单一静态 inverse LUT。

#### 4F. Profile 与板端执行

Calibration tool 生成版本化 `GamePlantProfile`：

```text
identity + SHA-256
plant_class: static_monotonic | dynamic_repeatable | non_stationary
HIP/ADS and sector tables
deadzone enter/exit and maximum rate
steady inverse knots
optional hold-time buckets or delay/time constants
measurement uncertainty / valid range
raw evidence manifest
```

左板只加载大小、单调性、范围、mode/sector 数和 hash 全部验证通过的 profile。
`stick_rate` 与 `MOVE_COMPAT` 共享同一个 inverse plant evaluator；错 hash、未知 mode、
超范围结构或 `NON_STATIONARY` profile 必须拒绝武装。动态 profile 的 hold timer 在
零、方向反转、mode 切换、TTL、override 和 hotplug 时清零。

Plant evaluator 必须有 host tests：所有 knot/sector/bucket 边界、正负端点、插值、
方向 wrap、零/反转 reset、错 mode/hash、NaN/溢出输入、饱和和 profile fuzz；运行时
不得分配内存。V1 禁止在正式游戏过程中在线学习或改写 LUT，所有更新必须离线生成、
版本化并重新经历 shadow/硬件门禁。

#### 4G. Secondary rate-intent shadow 与启用

- 新 AngularIntent 在 shadow 模式与 legacy `want_dx/dy` 同时记录；
- mouse live output 不改变；
- replay 检查 rate 的方向、限幅、step、jerk、停止和 confidence gate；
- 先以 recorded intent 驱动离线 plant，再做不射击训练场 live test；
- direct-stick ego/Smith 先 shadow，只有 emitted-stick plant prediction 通过才启用。

#### 退出门禁

- HIP/ADS、正负 X/Y、对角线及动态分类均有 quality-passed 证据；
- calibration blob 有 version、identity、class、SHA-256 和原始数据引用；
- `stick_rate` 只接受 `STATIC_MONOTONIC` 或达标的 `DYNAMIC_REPEATABLE` profile；
- requested rate、accepted seq、emitted stick、measured rate 可在同一时间轴对齐；
- 完整曲线证据来自 quality-passed wide/full-frame/telemetry，中心 ROI 只标记为 sanity；
- observation manifest、帧内 SEI、三时钟仿射映射及 uncertainty 均可离线重算；
- HIP/ADS 全流程由 calibration actuator 自动完成，所有 trial 都带 verified mode；
- abort/kill/串口拔出/视觉 mode 失配均成功中心并 release ADS，release 失败会 detach；
- AngularIntent shadow 回放通过且没有改变 mouse live baseline；
- 全部 transport/TTL/codec 硬门禁继续通过。

### 阶段 5：Legacy `km.move` 适配器

#### 实施内容

- 28 ms timestamped impulse window；
- 独立 lease；
- 10 ms rate-domain low-pass；
- on/off hysteresis；
- 方向反转清同轴窗口；
- `compat_estimated_cps` 与 `compat_requested_rate_mdeg_s` 分单位 telemetry；
- 与显式 state/rate 强制互斥；
- `CompatIntentCurve H(counts_per_second) -> requested_camera_rate`；
- requested rate 再进入阶段 4 的同一 inverse plant evaluator，禁止直接乘 Q15 gain。

`H` 的来源按优先级为：

1. 若有旧 mouse/XIM 基线，回放确定性 move 密度并测量相机角速度，拟合
   `H_legacy(cps)`，使 direct-stick 与旧路径在相同 move trace 下具有可比较语义；
2. 若最终完全不使用 XIM，则由 Secondary 的 AngularIntent/配置明确规定
   `mdeg/s per cps` 与限幅；
3. 两者都没有时 `MOVE_COMPAT` 不得宣称等价，只能保持 disabled。

Secondary 内部正式 direct-stick 路径直接发送 `stick_rate`，不应先量化成
`km.move` 再由板端反推；MOVE_COMPAT 只服务仍会发 legacy `km.move` 的调用方。

#### 回放输入

- 现有 wire trace 中低速稀疏 `move(1)`；
- 36/72/144/288 counts/s；
- 1000 counts/s 附近；
- 10368 counts/s 满量程；
- `dx/dy` 同时变化和对角线；
- 正负快速反转；
- move 停止但 lease 持续；
- lease 停止但最后 move 非零；
- UART burst、gap 和重复行。

#### 退出门禁

- 孤立 `move(1)` 默认不产生非零 stick；
- 平稳输入的 estimated rate 中段误差 `<=5%`；
- 低速无 1-poll PWM；
- 方向反转不携带旧方向窗口；
- lease 到期按 TTL 门禁回中；
- `source_conflict == 0`；
- 明确标为 compatibility，不替代正式 rate 质量结论；
- 持续的非零 estimated rate 通过 validated inverse plant 得到 stick；孤立
  `move(1)`、过期 lease 和低于 activation-on 的稀疏输入保持 synthetic 为零。
- `H` 与 game plant 使用不同 hash/version，任一变化都使组合 mapping 失效；
- 相同 deterministic move trace 的 requested rate 可重复误差 `<=5%`，最终相机
  速度误差按阶段 4 plant class 的门限判定。

### 阶段 6：物理合并和训练场 A/B

#### 顺序

1. physical-priority；
2. neutral-only synthetic；
3. physical takeover/release；
4. mouse/XIM 与 direct stick 等角速度 A/B；
5. HIP/ADS 切换；
6. 低速、step、反转、对角线、饱和和命令丢失；
7. 全部门禁通过后才实验 additive-with-fade。

#### Physical-priority 门禁

- 物理越过 on 门限后最迟下一 report synthetic 为中心；
- override 解除后旧命令不恢复；
- 必须新 seq 才重新激活；
- 1000 次 takeover/release 无松杆跳变；
- override 期间物理原始方向、幅度和按钮保持正确；
- override 指标与 USB capture 可一一对应。

#### A/B 判定

Mouse/XIM 和 direct-stick 必须使用相同：

- 游戏/FOV/FPS/灵敏度；
- 目标速度和场景；
- 测量工具和时间窗；
- arm/ADS 条件。

分别报告：

- command-to-camera delay p50/p95/p99；
- steady-rate error；
- overshoot、settle time、反转 delay；
- near-deadzone limit cycle；
- direction error；
- transport/USB counters。

不要求 direct-stick 必须优于 XIM 才算功能正确，但必须满足安全和可预测性门禁；
性能结论单独给出。

## 16. 测试矩阵总表

| 类别 | 场景 | 必看证据 | 通过条件 |
|---|---|---|---|
| Raw 回归 | OFF/mouse 10 min | USB report diff、status window | 零非预期字节变化，transport 增量 0 |
| Codec | `10^7` 离线报告 | host test manifest | 零越界/wrap/guard error |
| 正常 state | Q15 单轴/对角 | command、telemetry、USB capture | 字段/方向/保持/回中正确 |
| TTL | 停止命令 | accept/emission timestamp | `<= TTL + 1 interval` 回中 |
| Seq/session | duplicate/stale/wrong | counters、USB capture | 精确拒绝、零应用 |
| Hotplug | 下游 3 轮 | session、PnP、status | 旧状态不跨 session，自动恢复 raw |
| Stream stall | bridge report 停止 | fault/detach timeline | 不无限保持旧 synthetic |
| Mode switch | 100 轮 | ACK、emission、ledger | 屏障完成后 ACK，无旧状态 |
| Physical override | 1000 轮 | physical/merged telemetry | 下一 report 抑制，fresh-command 恢复 |
| Plant | HIP/ADS 八方向、duration/step/ramp | raw trials、class/model、quality | 分类正确且交叉验证达到对应门限 |
| 稳定性 | 2 h active | before/after counters | 零 drop/stall/re-enum/seq error |
| Host delivery | Windows/XInput/Raw Input | host harness | 单独报告 gap，不用板端结果掩盖 |
| 剩余 P4 | reset/sleep/four-IN | USB/PnP/status | 按现有权威文档独立验收 |

## 17. 证据包和可重复性

每次宣称 PASS 必须有 manifest：

```text
test_id / UTC-local timestamp
scenario and expected result
firmware SHA-256: left/right
source baseline / dirty-worktree note
controller descriptor/profile SHA-256
codec version and calibration SHA-256
Secondary config hash
game/FOV/FPS/sensitivity/response settings
command trace hash
USB capture / video / raw report file hashes
before/after LEFT_STATUS and RIGHT status
host harness result
metric verdicts
PASS/FAIL/INCONCLUSIVE and reason
```

建议统一存放：

```text
diagnostics/stick/<phase>/<test_id>/manifest.json
diagnostics/stick/<phase>/<test_id>/status_before.txt
diagnostics/stick/<phase>/<test_id>/status_after.txt
diagnostics/stick/<phase>/<test_id>/commands.csv
diagnostics/stick/<phase>/<test_id>/emissions.csv
diagnostics/stick/<phase>/<test_id>/usb_capture.*
diagnostics/stick/<phase>/<test_id>/video_metrics.json
```

## 18. 第一版默认配置

以下仅为保守 bring-up 起点，不是已标定值：

| 参数 | 默认 |
|---|---:|
| startup mode | `MOUSE_RELATIVE/raw` |
| command update | 144 Hz，包含显式零 |
| command TTL | 32 ms |
| state unit | Q15 normalized |
| rate unit | mdeg/s，仅 valid calibration |
| plant model | 未标定时 disabled；static LUT 优先，dynamic time-bucket 需单独通过 |
| synthetic max 初始/上限 | 0.20 / 0.35 |
| provisional game inner/outer deadzone | 0.12 / 0.02，必须用 synthetic plant 扫描替换 |
| rate LPF tau | 8 ms |
| compat window / LPF | 28 ms / 10 ms |
| compat activation | on 0.03 / off 0.02 |
| attack/release slew | 6.0 / 12.0 normalized/s |
| fault release | 下一新鲜 report 直接中心 |
| merge | physical-priority / neutral-only |
| override on/off/stable | 0.10 / 0.06 / 20 ms |
| reversal | 清同轴历史，中心 1 report |
| temporal dithering | off |
| HIP/ADS | 独立 profile/LUT/delay |
| stream watchdog | `max(20 ms, 4*p99 gap)`，bring-up cap 100 ms |

特别注意：许多游戏的实际 deadzone 可能高于 0.12。阶段 3 只验证字段；阶段 4
必须用自动 synthetic `stick_state_q15` 扫描 `0.05/0.10/0.15/0.20/0.30/0.35`
并细化临界区，不得把 0.12 硬编码为普适起转点，也不得要求用户用实体摇杆生成
这条曲线。

## 19. 建议代码落点与依赖顺序

### 左板 IDF

建议新增独立模块：

```text
components/makcm_core/include/stick_protocol.h
components/makcm_core/include/stick_session.h
components/makcm_core/include/controller_codec.h
components/makcm_core/include/stick_synth.h
components/makcm_core/include/game_plant_profile.h
components/makcm_core/include/game_plant_evaluator.h
components/makcm_core/include/move_compat_estimator.h
components/makcm_core/include/controller_calibration_actuator.h
components/makcm_core/stick_protocol.cpp
components/makcm_core/stick_session.cpp
components/makcm_core/controller_codec.cpp
components/makcm_core/stick_synth.cpp
components/makcm_core/game_plant_evaluator.cpp
components/makcm_core/move_compat_estimator.cpp
components/makcm_core/controller_calibration_actuator.cpp
```

现有文件的最小接入：

- `secondary.cpp`：解析/校验新命令，保持旧命令；
- `app_main.cpp`：profile/session lifecycle 和状态查询接线；
- `endpoint_router.cpp`：在 FIFO head 复制到 compose 后调用纯 mutation policy；
- `metrics.*`：新增明确指标，不改变已有指标语义；
- `CMakeLists.txt`：登记模块。

### Linux Secondary

建议：

```text
aim/output/ControllerStickOutput
DirectStickScheduler or ContinuousActuationPublisher
DirectStickCalibration profile loader
AngularIntentController (shadow first)
AppliedActuation telemetry receiver
CalibrationModeOrchestrator + visual mode verifier
standalone direct-stick calibrator
```

保留 `MouseRelativeOutput` 和现有 `XimReportScheduler`，不要用条件分支把两种
单位混在同一 scheduler 内。

### 实现依赖顺序

```text
one-time adapter qualification/profile
  -> protocol/session tests
    -> codec host tests
      -> compose-only integration
        -> state hardware bring-up
          -> standalone plant harness + classification
            -> plant evaluator host tests
              -> AngularIntent shadow
                -> rate backend
                  -> legacy move adapter
                    -> physical merge/A-B
```

任何阶段失败时回到最近一个已通过阶段，不跳过安全门禁继续叠加功能。

## 20. 回退与发布策略

- Firmware 默认启动仍为 raw/mouse；没有 Secondary 握手不进入 stick。
- Stick feature 使用独立 compile-time/runtime capability flag。
- 关闭 capability 后生成的 transport 行为必须与阶段 0 基线一致。
- 新 codec 逐个 report-format adapter/identity allow-list 启用，不做 generic wildcard；
  同一已验证 identity 的不同实体设备复用 profile，不做逐台用户校准。
- 任何 mutation/profile/stream fault 自动回 raw-safe 或 detach，不自动重试非零。
- 发布包记录左右板 firmware hash、支持的 format/calibration hash 列表。
- 不因 direct-stick 上线改变当前尚未完成的 P4 项目结论；USB reset、睡眠唤醒、
  四路 IN 仍按权威实施状态文档独立追踪。

## 21. 完成定义

只有同时满足以下条件，才能把 direct-stick V1 标记为完成：

1. 阶段 0–6 的所有适用门禁为 PASS，无 INCONCLUSIVE 冒充；
2. OFF/mouse/raw 行为与权威基线一致；
3. Generic endpoint 保持独立有序 FIFO、公平 drain 和现有退休语义；
4. 至少一个 Xbox 风格 report adapter 和一个 DualSense Edge report adapter
   完成 codec、状态、TTL、hotplug 和 2 小时稳定验收，且同 identity 换设备不需
   重新扫轴或生成 profile；
5. HIP/ADS direct plant 均有 quality-passed、可追溯 calibration；
6. 所有旧 session/seq/profile mismatch/TTL/stream-stall 故障均 fail-closed；
7. Unknown profile 在所有回放和硬件窗口中零修改；
8. Linux 能区分 requested、board-accepted 和实际 emitted actuation；
9. Legacy `km.move` 仍可在 mouse 模式工作，并能显式切换回来；
10. 发布 manifest、firmware/config/profile/calibration hashes 和测试证据齐全。
11. 完整 game plant（deadzone、稳态曲线、方向和时间动态）全部由 synthetic 激励
    生成，不依赖实体摇杆中心或单轴人工动作。
12. 当前 phase-3 `virtual counts -> Q15` 线性缩放未被当作正式映射；正式 direct
    backend 使用 AngularIntent 和通过交叉验证的 game plant profile。
13. `NON_STATIONARY` plant 不得以静态 LUT 冒充通过，`stick_rate`/MOVE_COMPAT
    必须拒绝或明确处于实验闭环模式。
14. 正式 HIP/ADS profile 的 mode transition 全部由 calibrator 自动执行并验证；
    人工按键 trial 不得进入发布 calibration，任何异常都会中心并 release ADS。
15. 完整 plant 不以生产中心 ROI 的局部 global-SAD 为唯一观测；正式证据使用带几何
    manifest、逐帧 QPC SEI 和三时钟不确定度门禁的 wide/full-frame/telemetry 链路。
