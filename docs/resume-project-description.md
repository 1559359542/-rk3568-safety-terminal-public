# 简历项目描述：RK3568 端侧 AI 工业安全监测与 HMI 告警终端

## 一句话项目简介

基于 RK3568 Linux 搭建端侧工业安全监测原型，打通设备树/驱动、I2C/UART/GPIO/PWM 采集与控制、USB V4L2 视频、RKNN+ROI 视觉告警、Qt/QML HMI、SQLite 记录和 MQTT 上报全链路。

## 简历项目职责 / 成果

- 负责 RK3568 Linux BSP 集成与设备树设计，使用 DTS/DTSI 配置 GPIO、PWM、I2C、UART、MIPI 等资源；实现 `safety_event` platform/字符设备驱动，支持门磁中断、50 ms 消抖、`read/poll/ioctl` 用户态接口。
- 集成 SHT30（SHT3x 兼容链路）、BH1750、LD2410 毫米波雷达、门磁和 USB 摄像头；围绕 I2C、UART、GPIO、PWM、UVC/V4L2 完成采集、状态快照和本地声光告警。
- 基于 RKNN 运行量化 YOLOv5，结合 RGA 预处理和固定危险区域 ROI，按人员检测框底边中心点生成进入/清除事件；通过 `ai_alarm_bridge` 接入统一告警规则。
- 使用 Qt/QML 构建监控、告警中心、环境监测、历史记录和系统诊断 HMI；以 SQLite 保存事件，以 MQTT 上报事件，形成可追溯的状态闭环。
- 在 RK3568 EVB 上验证 USB 摄像头 `1280x720 MJPG` 输入和约 `29.99 FPS` MPP H.264 预览，完成 AI ROI、门磁、雷达、温度超限/恢复及历史事件证据采集。

## 技术栈

RK3568、Linux 4.19、Buildroot、Device Tree/DTSI、C/C++、GPIO/PWM、I2C、UART、USB UVC/V4L2、RGA、RKNN、Qt/QML、SQLite、MQTT、systemd。

## 面试时 1 分钟项目介绍

这是我基于 RK3568 Linux 做的端侧 AI 工业安全监测与 HMI 告警终端。底层先通过项目 DTS/DTSI 描述 GPIO、PWM、I2C、UART 和 MIPI 资源，再由 `safety_event` 字符设备驱动提供门磁中断和 `read/poll/ioctl` 接口。用户态采集 SHT30、BH1750、LD2410 和门磁状态；USB 摄像头通过 OpenCV 的 V4L2 backend 获取视频，经 RGA 缩放后送入 RKNN YOLOv5，只筛选人员并用检测框底边中心点判断是否进入固定危险区域。AI、雷达、门磁和温度告警在 `safety_alarmd` 中统一合并，驱动 GPIO/PWM 输出声光告警，同时生成统一 JSON 状态，SQLite 记录历史，MQTT 上报，Qt/QML HMI 展示实时状态和事件。板端证据中摄像头为 1280x720 MJPG，预览链路约 29.99 FPS；我也验证了 ROI 进入/清除、门磁恢复、雷达有人/无人和温度超限恢复等场景。这个项目重点体现的是 BSP、设备树、驱动接口和多进程系统集成能力，AI 是其中一个受边界约束的告警输入。

## 简历链接写法

项目源码：<https://github.com/1559359542/-rk3568-safety-terminal-public>  ·  在线展示：<https://1559359542.github.io/-rk3568-safety-terminal-public/>
