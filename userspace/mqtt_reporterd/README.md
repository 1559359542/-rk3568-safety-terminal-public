# mqtt_reporterd

RK3568 工业安全终端的 MQTT 只读上报服务。它只读取系统状态快照和
SQLite 事件库，不订阅控制主题，也不修改 HMI、告警、AI、GPIO 或 PWM。

## 数据流与主题

```text
system_state.json -> status JSON -> device/<device_id>/status
SQLite events     -> event JSON  -> device/<device_id>/event
process state     -> online/offline -> device/<device_id>/availability
```

- `availability`：QoS 1、retain。
- `status`：每 5 秒发布，QoS 1、retain；使用 JSMN 按 JSON 层级解析。
- `event`：按 `event_id` 升序发布，QoS 1、不 retain；成功发布后才推进游标。
- 每条事件同时包含结构化字段和一条完整中文摘要，摘要串联时间、来源、
  事件类型、等级、激活状态、数值和阈值，便于 MQTT 客户端直接展示。
- 缺失的传感器数值发布为 JSON `null`，不会伪装成 `0`。
- 人员入侵由 AI 与雷达告警融合；只有两个来源均健康且均明确为 false
  才发布 false，信息不足时发布 `null` 和“状态未知”。
- 快照或 SQLite 暂时不可用只跳过相应消息，不会导致 MQTT 断线重连。

## 构建

在 Ubuntu 项目根目录执行：

```sh
make -C userspace/mqtt_reporterd
```

Makefile 使用 Buildroot host sysroot，并链接 Paho MQTT C 与 SQLite。

## 板端部署布局

当前板端根文件系统没有 `libpaho-mqtt3c.so.1`，因此必须同时部署私有库：

```text
/opt/mqtt_reporterd/mqtt_reporterd
/opt/mqtt_reporterd/lib/libpaho-mqtt3c.so.1.2.0
/opt/mqtt_reporterd/lib/libpaho-mqtt3c.so.1 -> libpaho-mqtt3c.so.1.2.0
/etc/init.d/S95mqtt_reporterd
/etc/default/mqtt_reporterd
```

启动脚本会把 `/opt/mqtt_reporterd/lib` 加入本进程的
`LD_LIBRARY_PATH`，不会修改全局动态库目录。

## 配置与历史事件策略

将 `mqtt_reporterd.env.example` 安装为
`/etc/default/mqtt_reporterd`。默认 `MQTT_REPLAY_HISTORY=0`：首次启动且没有
有效游标时，服务先记录数据库当前最大的 `event_id`，只上报之后产生的新事件。
设置为 `1` 可在首次启动时从 `event_id=0` 开始补传全部历史事件。已有有效游标时
始终从游标继续，避免重启后重复上报。

游标文件为：

```text
/userdata/industrial_safety/.mqtt_reporterd_last_event_id
```

## 范围限制

本服务没有 MQTT subscribe 调用和远程控制路径。MQTT Broker 故障只影响远程
展示，不影响板端预览、预警、历史记录和本地告警链路。
