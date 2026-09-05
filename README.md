# 基于 RK3568 Linux 的端侧 AI 工业安全监测与 HMI 告警终端

一个面向学习、面试和家庭环境验证的 RK3568 Linux 嵌入式演示原型。系统将设备树、Linux 驱动、传感器采集、端侧视觉推理、告警规则、事件记录、MQTT 上报和 Qt/QML HMI 组织成一条可解释的数据链路。

> 本项目是辅助监测和异常检测演示原型，不是经过认证的工业安全系统。AI 结果用于疑似事件提示，不作为安全关键决策。

## 项目主线

```text
DTS/DTSI
  -> safety_event platform driver
  -> /dev/safety_event
  -> safety_alarmd
  -> state_snapshotd
  -> system_state.json
  -> SQLite / MQTT / Qt-QML HMI
```

AI 支线：

```text
USB UVC/V4L2 -> RGA -> RKNN -> 人员检测与危险区域 ROI 判断
  -> ai_alarm_bridge -> safety_alarmd -> GPIO/PWM 告警
```

## 核心技术贡献

- 使用项目 DTS/DTSI 描述 GPIO、PWM、I2C、UART 和 MIPI 资源；
- 设计 `safety_event` platform driver，完成 `probe/remove`、字符设备注册和资源初始化；
- 支持门磁 GPIO 中断、50 ms 消抖、wait queue、mutex 以及 `read/poll/ioctl` 用户态接口；
- 使用 PWM 和 GPIO 控制本地告警输出，合并门磁、低照度和用户态告警状态；
- 通过 I2C 读取 BH1750 光照数据，配合 SHT3x、LD2410B 用户态采集链路；
- 通过 V4L2 获取 USB 摄像头数据，使用 RGA/RKNN 完成人员检测和固定 ROI 判断；
- 由状态快照服务统一输出 JSON，供 SQLite、MQTT 和 Qt/QML HMI 读取。

## 目录说明

| 目录 | 内容 |
| --- | --- |
| `drivers/` | `safety_event` 和示例内核模块源码 |
| `userspace/` | 告警服务、传感器解析、AI 桥接、SQLite、MQTT、HMI |
| `boards/` | RK3568 项目设备树 |
| `dts_patches/` | 项目设备树补丁和覆盖层 |
| `kernel_config/` | 内核配置片段 |
| `kernel_patches/` | 内核侧补丁 |
| `scripts/` | DTS 同步、构建和配置脚本 |
| `tests/` | 用户态和接口测试源码 |
| `docs/` | 引脚、接口和使用说明 |

## 关键接口

| 边界 | 接口 | 作用 |
| --- | --- | --- |
| 内核/用户态 | `/dev/safety_event` | 告警事件读取和控制 |
| 事件通知 | `poll()` + wait queue | 异步事件唤醒 |
| 参数配置 | `ioctl()` | 设置用户态告警和清除告警 |
| 状态快照 | `/run/industrial_safety/system_state.json` | HMI、SQLite、MQTT 的统一只读输入 |
| 摄像头 | `/dev/video*` | USB UVC/V4L2 视频输入 |
| 雷达 | `/dev/ttyS9` | LD2410B UART 数据输入 |

## 验证状态

项目材料按以下等级区分，避免把计划当成板端结论：

```text
源码存在 != 编译成功
编译成功 != 已部署
已部署 != 板端运行
板端运行 != 异常场景已验证
```

当前公开版保留源码、配置、脚本和接口说明；板端日志、网络环境、构建产物和私有 U-Boot 证据未放入公共仓库。后续将在 `docs/test-evidence.md` 中只补充可公开、可复现的验证记录。

## 构建与部署边界

项目依赖目标板的 RK3568 SDK、Linux 4.19 内核、Buildroot rootfs、交叉工具链、RKNN/RGA 和 Qt 运行环境。公开仓库提供源码和脚本，不承诺在任意主机直接编译通过。

推荐验证顺序：

1. 检查 SDK 内核版本、架构和配置；
2. 使用项目脚本同步 DTS/DTSI 并构建 DTB；
3. 使用外部模块方式构建 `safety_event.ko`；
4. 在板端确认 `/dev/safety_event`、`/dev/video*` 和 `/dev/ttyS9`；
5. 启动告警服务和状态快照服务；
6. 再启动 HMI、SQLite 和 MQTT 消费者。

## 演示建议

推荐演示一条完整事件链：

```text
触发门磁/低照度/人员 ROI 事件
  -> 驱动或用户态采集
  -> safety_alarmd 规则判断
  -> LED/PWM 本地告警
  -> system_state.json 状态更新
  -> SQLite 记录 / MQTT 上报 / HMI 展示
```

项目网页使用 `assets/照片素材/` 中的现场照片、HMI 截图、日志截图和板端证据包；本次不制作演示视频。

## 当前视觉证据

`assets/照片素材/` 当前直接保留全部现场图片，不做筛选、裁剪或脱敏。图片用于项目网页和个人展示时，应结合对应板端证据目录阅读。

### 根目录

- [总览包含摄像头位置](assets/照片素材/总览包含摄像头位置.jpg)
- [板端 HMI、传感器与自制电源](assets/照片素材/板端hmi以及传感器，自制电源.jpg)
- [MQTT 门磁打开关闭上报](assets/照片素材/MQTT事件上报-门磁打开关闭-脱敏.png)

### HMI 界面

- [监控界面](assets/照片素材/hmi界面/监控界面.jpg)
- [告警中心](assets/照片素材/hmi界面/告警中心.jpg)
- [环境监测界面](assets/照片素材/hmi界面/环境监测界面.jpg)
- [历史记录界面](assets/照片素材/hmi界面/历史记录界面.jpg)
- [系统诊断界面](assets/照片素材/hmi界面/系统诊断界面.jpg)

### 门磁

- [门磁闭合](assets/照片素材/门磁/门磁闭合.jpg)
- [门磁告警、蜂鸣器与 LED](assets/照片素材/门磁/门磁告警蜂鸣器响led点亮.jpg)
- [门磁恢复历史记录](assets/照片素材/门磁/门磁恢复历史记录.jpg)
- [门磁打开历史记录](assets/照片素材/门磁/门磁打开历史记录.jpg)
- [门磁打开现场](assets/照片素材/门磁/门磁打开图片.jpg)

### 温湿度

- [SHT30 传感器](assets/照片素材/SHT30温湿度传感器/SHT30温湿度传感器.jpg)
- [温湿度显示](assets/照片素材/SHT30温湿度传感器/温湿度显示.jpg)
- [高温告警](assets/照片素材/SHT30温湿度传感器/高温告警.jpg)
- [高温告警恢复](assets/照片素材/SHT30温湿度传感器/高温告警恢复.jpg)
- [温湿度历史记录](assets/照片素材/SHT30温湿度传感器/历史记录.jpg)

### 雷达

- [LD2410 毫米波雷达](assets/照片素材/LD2410毫米波雷达/LD2410毫米波雷达图片.jpg)

### 摄像头与 AI

- [摄像头](assets/照片素材/摄像头/摄像头.jpg)
- [人员进入 ROI](assets/照片素材/摄像头/人员进入ROI.jpg)
- [AI 告警](assets/照片素材/摄像头/AI告警.jpg)
- [人员闯入历史事件刷新](assets/照片素材/摄像头/人员闯入历史事件刷新.jpg)

板端运行证据位于 [`assets/运行证据/20260905/`](assets/运行证据/20260905/)。其中包含正常状态、门磁、雷达、温湿度、AI、历史记录等场景的文本和 JSON 证据。传输用的压缩包不作为仓库内容提交。

## 交互式项目说明

- [RK3568 项目全链路阅读器](docs/12-RK3568项目全链路阅读器.html)

## 进一步阅读

- `docs/pin-plan.md`
- `drivers/safety_event/safety_event.c`
- `drivers/safety_event/safety_event_uapi.h`
- `userspace/safety_alarmd/`
- `userspace/ai_yolov5/`
- `userspace/hmi_qml/`

## License

待根据依赖和公开范围补充许可证说明。
