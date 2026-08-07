# Direct-stick report-format adapter qualification 流程

本流程是“新增或变更 controller report 格式时的一次性开发取证”，不是最终用户
为每只手柄执行的校准流程。它只建立右摇杆字段、编码和完整性规则证据；游戏
deadzone、角速度曲线和 `km.move` 映射由 synthetic plant 标定单独完成。

## 1. 什么时候需要执行

只有以下情况需要 adapter qualification：

- 新增尚未支持的 controller report 协议；
- descriptor hash、Report ID、长度、字段布局或完整性规则发生变化；
- 同 VID/PID 出现无法由现有 adapter 解释的新 firmware/transport variant；
- 需要证明直连 persona 与板端代理后的 persona 是否仍使用同一 report grammar。

以下情况不应要求重新执行：

- 换成同一受支持 descriptor/report identity 的另一只实体手柄；
- 单台手柄存在轻微中心漂移；
- 游戏、FOV、灵敏度或 ADS 设置改变；
- 用户只在 `RAW_PROXY` 模式使用透传。

Profile/adapter 的复用粒度是 report identity，不是设备序列号。V1 不使用运行时
自动中心去重写物理 report；未被 synthetic 接管时，原始 bytes 保持不变。

## 2. 必须保存的协议证据

对每个新的 report identity 保存：

- 18-byte Device Descriptor；
- 完整 active Configuration Descriptor；
- 目标 controller interface 的完整 HID Report Descriptor（如存在）；
- 目标 Interrupt IN endpoint 的完整 report；
- VID、PID、bcdDevice、interface、endpoint、Report ID、实际 report length；
- neutral 与带时间标记的 guided motion trace；
- 其他 sticks/triggers/buttons 的排除 trace；
- 抓包工具版本、接线方式、左右板 firmware hash 和采集时间。

若右板 adapter 会改变 interface/endpoint 或 descriptor，直连设备与板端代理 persona
必须分别保存。DualSense Edge controller-only adapter 后的虚拟 interface/endpoint
不得根据物理 `IF3/EP84` 猜测。

## 3. 推荐人工动作：方向锚点加圆周

不再要求操作者完成严格 X-/X+/Y-/Y+ 单轴直线。一次连续动作即可：

1. 手柄平放并完全松开右摇杆 2 秒；
2. 将右摇杆推到物理“右”并保持 1 秒，回中；
3. 将右摇杆推到物理“上”并保持 1 秒，回中；
4. 沿外圈顺时针旋转 2 圈；
5. 沿外圈逆时针旋转 2 圈；
6. 完全松开并静置 2 秒。

动作允许弧线、交叉轴和手抖。右/上锚点用于确定 X/Y 和 sign；双向圆周用于识别
端点、非对称量程、椭圆度和轴耦合。操作者只需明显完成方向和整圈，不需要画出
实验室级水平/垂直直线。

推荐每个新格式使用两台同 identity 的实体设备重复上述动作。第二台必须复用同一
adapter；若必须重新生成中心/端点才能通过，则该 adapter 尚未达到可发布条件。

## 4. Guided trace 合同

推荐统一 CSV：

```csv
timestamp_s,endpoint,report_hex,marker
0.000000,0x84,018080...,neutral_begin
2.000000,0x84,018080...,right_begin
3.000000,0x84,01ff80...,right_end
4.000000,0x84,018000...,up_anchor
5.000000,0x84,018080...,cw_begin
```

`marker` 可以为空；关键动作边界由采集端热键/控制命令写入，不能依赖事后凭感觉
切片。`report_hex` 必须包含完整 interrupt payload；有 Report ID 时保留首字节。
只导入有 payload 的 IN completion，不能把 submit 和 completion 重复计入。

Analyzer 应使用稳健统计而不是“交叉轴必须小于 10%”的人工动作门槛：

- neutral 取 median、MAD、p99/p99.9 jitter；
- 从所有字段的时序相关性筛出候选轴；
- 用右/上锚点确定 axis identity 和 sign；
- 用双向圆周的分位数/稳健椭圆拟合估计端点 envelope；
- 排除按钮、trigger、counter、timestamp 和 checksum 字段；
- 输出每项判定的置信度和拒绝原因，低置信度不得生成 firmware profile。

Qualification 结果只能固化协议常量与允许 envelope；不能生成“这只手柄运行时应
被校正到哪个中心”的逐台配置。

## 5. 当前工具与迁移状态

`tools/stick_profile_tool.py` 现在保留两个明确隔离的入口：

- legacy bring-up：neutral、X-/X+、Y-/Y+ 五段 CSV，保留 10% 单轴隔离门槛和 C++
  条目输出，只服务既有证据回放；
- `--guided-trace`：使用第 4 节单一 marker CSV，自动筛选 `uint8/int16_le` X/Y 字段，
  允许自然交叉轴动作，并验证右/上锚点、顺/逆圈数、方向和圆周覆盖。

Guided path 使用 median、MAD、p99/p99.9 jitter、稳健分位 envelope、椭圆度、角度
展开与 16-bin coverage；counter、timestamp/checksum-like、discrete、重叠和歧义字段
必须被拒绝。顺/逆圆周各要求至少 1.25 个有效外圈 winding，目的是容忍手工 marker
和回中误差，同时拒绝只画一圈或残缺圆周。

Guided 成功只写待评审 candidate JSON，不直接写 firmware C++；指定 `--out-cpp` 会
失败且不生成任何输出。JSON 中观测中心/jitter/envelope 与协议 center/min/max 假设
分开记录，并明确 `runtime_per_device_center_correction=false`。

### 5.1 从 USBPcap 到 guided CSV

`tools/guided_usb_capture.py` 是采集入口，Wireshark/USBPcap 必须由操作者明确安装和
授权，工具不会自动安装。当前开发机已安装 `tshark/USBPcapCMD`，重启后 USBPcap
kernel service 和 root-hub interface 已就绪；下列 live 步骤仍等待首次实机证据。

0. 先运行只读环境门禁。它把 live capture 与 descriptor snapshot 依赖分开报告，
   不会安装软件、重启系统、选择 root-hub/device/endpoint 或自动开始抓包：

```text
python tools/guided_usb_capture.py preflight
  --out-json diagnostics/ds02_preflight.json
```

退出码为 `0=全部就绪`、`3=抓包依赖缺失`、`4=descriptor 依赖缺失`、
`5=两组均缺失`。只有退出码为 `0` 才进入下面的首次 live 流程。当前开发机已安装
Wireshark 4.6.7；PyUSB 1.3.1 与 `libusb-package` 1.0.30.0 因 bundled runtime 的
sandbox ACL 使用 workspace-local target，运行时先设置：

```powershell
$env:PYTHONPATH = (Resolve-Path diagnostics\ds02_install\python_site).Path
```

在该环境下 descriptor 与 capture 依赖全部 PASS，当前结果为 `0`。USB address 会在
热插拔/重新枚举后变化，旧证据中的 device address 不得复用；当前一次热插拔后的
观测地址为 `11`，仍必须由紧邻正式抓包的 extcap 枚举再次确认。环境 PASS 不是设备、
端点或 adapter qualification PASS。preflight 强制 USBPcap >= 1.5.4.0，以排除旧版
driver 的 endpoint handle-reuse 错误。

1. USBPcapCMD 通过 extcap 只读列出 root-hub interface；必须人工选择，工具不会自动选择：

```text
python tools/guided_usb_capture.py live --list-interfaces
```

2. 短抓包 discovery。期间只自然活动右摇杆；输出只列候选 bus/device/IN endpoint、
   report length、cadence 和变化字节，不自动选择：

```text
python tools/guided_usb_capture.py discover
  --interface \\.\USBPcap1 --duration-s 10
  --pcap evidence/discovery.pcap --tsv evidence/discovery.tsv
  --out-json evidence/discovery.json
```

每次 capture 都会重新运行 extcap interface/device 枚举；显式 interface 或 device
不在当次枚举中会直接失败。输出 `.pcap` 必须事先不存在（USBPcap 使用
`CREATE_NEW`），工具不接受 `.pcapng` 后缀或旧文件。正式 10 秒 discovery 的 pcap
时间范围必须至少为 9.75 秒且包含连续 payload-bearing interrupt-IN completion，
否则不得标为 PASS。

USBPcapCMD 启动后可能先做 descriptor injection。工具使用独立的 15 秒 readiness
期限。raw pcap 可读时，readiness 要求合法 little-endian classic pcap header 和至少
一个完整且严格校验通过的 USBPcap record。Windows USBPcapCMD 也可能用不允许共享读取
的方式独占打开输出；遇到 `PermissionError` 时，工具不会把 sharing violation 误报为
格式错误，而是只把本次新建文件达到 67-byte 最小 header/record 结构尺寸且随后继续
增长视为 operational readiness，并明确把内容验证标记为 deferred。停止捕获、文件锁
释放后仍无条件执行完整严格 parser、首 record、linktype 和所有长度校验；任何非法内容
仍 fail-closed。

只有上述 strict 或 operational readiness 成功后才显示
`Discovery capture running`、启动 requested-duration watchdog，live 也只有此后才
显示 `Guided capture running`、启动 watchdog 并接受首个 marker 提示。因此 USBPcap
初始化时间不占用 10 秒动作窗口。readiness 超时或 USBPcapCMD 在 readiness 前退出都会
受控清理本次进程树，不生成派生 PASS 文件。

历史 `diagnostics/ds02_4131/discovery.pcap` 可用严格离线回放检查 parser：

```text
python tools/guided_usb_capture.py inspect-pcap
  --pcap diagnostics/ds02_4131/discovery.pcap
  --tsv evidence/old-replay.tsv --out-json evidence/old-replay.json
  --expected-duration-s 10
```

该文件只覆盖约 1.486727 秒，因此结果固定为 `PARTIAL_SHORT_CAPTURE`，不能替代正式
discovery PASS。

3. 用 `tests/usb_descriptor_snapshot.py` 保存 descriptor JSON，再按目标 controller
   interface 解包。composite 设备必须选 controller interface，不能选鼠标 HID：

```text
python tools/guided_usb_capture.py descriptors
  --snapshot evidence/descriptors.json --target-interface <if>
  --out-dir evidence/descriptors
```

4. 人工评审 discovery 后，把明确的 bus/device/endpoint 传给 live。工具会逐段提示
   begin/end；操作者不需要保持单轴，只需按提示完成 neutral、右/上锚点和双向两圈：

```text
python tools/guided_usb_capture.py live
  --interface \\.\USBPcap1 --bus <bus> --device <device> --endpoint <ep>
  --pcap evidence/guided.pcap --markers-json evidence/markers.json
  --tsv evidence/guided.tsv --out-csv evidence/guided.csv
  --audit-json evidence/guided.audit.json
```

live/discovery 不依赖 `usb.urb_type` 或 `usb.capdata`：Wireshark 4.6.7 的 USBPcap
header 没有前者，后者还可能被 HID/XInput sub-dissector 消耗。工具直接严格解析
classic pcap/USBPcap pseudoheader，只导入 `irp_info.direction=1`、目标
bus/device、interrupt-IN endpoint 和完整 payload；tshark 只做读取兼容性/版本检查。
submit、OUT、非 interrupt、空包、pcap 截断/长度不一致、marker 碰撞/错序/超时都会
fail-closed。live 的 raw 时间必须覆盖全部 marker，最后 marker 后还必须有目标 report。
中止时保留本次新建的原始 pcap/partial marker，不生成派生 PASS 文件。

USBPcapCMD 父/worker 由独立 duration watchdog 通过本次 PID 的 Windows process tree
受控结束；开始前若已有 USBPcapCMD 会拒绝运行，结束后若本次 tree 仍有 PID 存活也会
失败。audit 记录 command/version、枚举、PID/exit、controlled stop、elapsed、
readiness method、是否 deferred、observed size、停止后严格验证结果、首 record 时间、
pcap 时间范围、parser schema 与所有证据 hash。

### 5.2 生成待评审 adapter candidate

目标 interface 是 HID 时使用它自己的 `hid_report_ifN.bin`；目标是 vendor/XInput 时
使用 `--no-hid-report-descriptor`，即使 composite 中另有鼠标 HID 也不得借用。
示意调用为：

```text
python tools/stick_profile_tool.py
  --format-id <id> --vid <vid> --pid <pid> --bcd-device <bcd>
  --interface <if> --endpoint <ep> --report-id <rid> --report-length <len>
  --device-descriptor device.bin --configuration-descriptor config.bin
  (--hid-report-descriptor hid_report_ifN.bin | --no-hid-report-descriptor)
  --guided-trace guided.csv
  --out-json adapter_candidate.json
```

Candidate 仍须人工评审完整性字段并完成 DS-02 实机取证，不能把工具输出当作“通用
手柄支持”证明。所有未知 identity 继续 `RAW_PROXY` 原样透传，`SUPPORTED_OVERLAY`
fail-closed。

Guided analyzer 的最低输出为：

```text
format_id / adapter_version
descriptor and target HID SHA-256, or explicit no-HID identity
interface / endpoint / Report ID / length
X/Y offset / encoding / sign
protocol center/min/max
qualification envelope and confidence
integrity fields/update rule
evidence paths and profile SHA-256
```

## 6. 完整游戏 plant 不在本流程中

游戏不能简化成一个 deadzone。必须使用板端自动发送 synthetic `stick_state_q15`
分别测量：

- deadzone enter/exit；
- 完整稳态非线性曲线和最大转速；
- 正负 X/Y、对角线和方向扇区差异；
- 50–1500 ms duration、step、ramp、triangle 的启动延迟、时间加速、惯性和回中；
- HIP/ADS、倍率、FOV、灵敏度、response preset 和 FPS 差异。

实体物理摇杆在这些测试中保持松开。Plant 必须分类为 `STATIC_MONOTONIC`、
`DYNAMIC_REPEATABLE` 或 `NON_STATIONARY`，并按 game/settings identity 保存模型和
原始证据，不能写回 controller adapter profile。

生产 Secondary 目前只收到 Host 居中的 `960x960 -> 640x640` ROI；该流以及现有
32 px global-SAD 只能做静止、方向和低速短脉冲 sanity，不能单独测完整曲线和最大
转速。正式 plant 流程必须让 Host 进入互斥的 `calibration-wide` 特别模式，把完整
游戏 client rect 等比例编码到专用 AimAU/meta 端口，或改用带 QPC 的 Windows 全帧
录像/可信 telemetry。生产 ROI 配置不因此改变。

每帧依靠帧内 SEI 的 `cal_session_id + frame_id + DXGI present QPC` 与视频严格绑定；
Host、Secondary、板端分别通过四时间戳交换和仿射时钟映射对齐。固定 FPS 编码 PTS、
UDP 到达时间、串口 write 时间都不能替代真实 capture/emission 时间。只有中心 ROI、
缺少 geometry manifest 或时钟 uncertainty 超门限时，完整 plant 结果必须标为
`PARTIAL/INCONCLUSIVE`。

HIP/ADS 切换也由 standalone calibrator 自动完成：先发中心、通过 adapter 声明的
controller trigger/button field 按下 ADS、等待并用视觉 FOV/reticle 或 telemetry
确认、执行 ADS trials，最后中心并自动 release。操作者不负责逐 trial 手动切换；
无法自动注入或确认 mode 时，ADS calibration 必须标为不支持/证据不足。

`MOVE_COMPAT` 的链路是：

```text
timestamped move density
  -> CompatIntentCurve (counts/s -> requested camera rate)
  -> validated game plant inverse
  -> stick Q15
```

它不能直接把 move count 乘固定 Q15 gain。孤立 `move(1)` 保持 synthetic 为零；
持续输入只有在 compat curve 与 game plant profile 都有效时才允许跨过游戏响应零区。

## 7. 固件启用边界

仓库默认可以没有本地 adapter 条目。没有匹配 identity 时：

- `RAW_PROXY` 和原有 mouse 路径继续工作；
- `SUPPORTED_OVERLAY` 的 begin 返回 profile/adapter mismatch；
- 所有 controller report 零修改；
- 不回退到 VID/PID wildcard 或猜测 offset。

发布时必须包含经过评审的 adapter manifest、原始证据 hash、允许 identity 列表、
固件 hash 和回放/实机测试结果，不能只分发不可追溯的二进制。
