import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtQuick.Window 2.15

ApplicationWindow {
    id: window
    visible: true
    visibility: Window.FullScreen
    title: "工业安全监控"
    color: "#101417"
    readonly property color bg: "#101417"
    readonly property color panel: "#1b2228"
    readonly property color line: "#3b4750"
    readonly property color text: "#edf2f5"
    readonly property color muted: "#aab5bd"
    readonly property color ok: "#63c98d"
    readonly property color warn: "#f0bd60"
    readonly property color danger: "#f27d73"

    function statusColor(active, stale) { return stale ? warn : (active ? danger : ok) }
    function stateText(value, good) {
        if (!runtime.valid) return "状态缺失"
        if (runtime.stale) return "数据过期"
        return value === good ? "在线" : value
    }
    function levelText(value) {
        if (value === "alarm") return "告警"
        if (value === "warning") return "注意"
        if (value === "info") return "信息"
        if (value === "clear") return "恢复"
        return value || "未知"
    }
    function sourceText(value) {
        if (value === "ai") return "AI 检测"
        if (value === "intrusion") return "入侵检测"
        if (value === "radar") return "雷达"
        if (value === "light") return "光照告警"
        if (value === "environment") return "环境联合告警"
        if (value === "temperature") return "温度传感器"
        if (value === "humidity") return "湿度传感器"
        if (value === "illuminance") return "光照传感器"
        if (value === "bh1750") return "光照传感器"
        if (value === "sht3x") return "温湿度传感器"
        if (value === "door") return "门磁"
        if (value === "system") return "系统服务"
        return value || "未知来源"
    }
    function eventTypeText(value) {
        if (value === "event") return "检测事件"
        if (value === "enter") return "进入区域"
        if (value === "clear") return "告警恢复"
        if (value === "overexposure") return "高亮度超限"
        if (value === "lux_low") return "低照度超限"
        if (value === "overheat") return "温度过高"
        if (value === "humidity_high") return "湿度过高"
        if (value === "suspected_fire") return "疑似火灾"
        if (value === "suspected_fire_clear") return "疑似火灾恢复"
        if (value === "open") return "门磁打开"
        if (value === "close") return "门磁关闭"
        if (value === "offline") return "服务离线"
        if (value === "recovery") return "服务恢复"
        if (value === "startup") return "服务启动"
        if (value === "shutdown") return "服务停止"
        if (value === "snapshot_invalid") return "状态快照异常"
        if (value === "snapshot_recovery") return "状态快照恢复"
        return value || "未知事件"
    }
    function eventTitle(source, type) {
        if (type === "recovery" || type === "clear" || type === "suspected_fire_clear") return sourceText(source) + "恢复"
        if (source === "intrusion" || type === "enter") return "人员闯入，靠近危险区域"
        if (source === "environment" && type === "suspected_fire") return "疑似火灾或机器故障事件"
        if (source === "environment" && type === "event") return "温度或光照异常"
        if (source === "temperature" || type === "overheat") return "传感器温度较高"
        if (source === "light" || type === "overexposure") return "周围光照较高"
        if (source === "door" && type === "open") return "门磁异常，门被打开"
        if (source === "door" && type === "close") return "门磁恢复，门已关闭"
        return sourceText(source) + "：" + eventTypeText(type)
    }
    function eventDescription(source, type, detail) {
        if (type === "recovery" || type === "clear" || type === "suspected_fire_clear") return "指标已回到安全范围"
        if (source === "intrusion" || type === "enter") return "请立即确认人员位置并远离危险区域"
        if (source === "environment" && type === "suspected_fire") return "温度与光照同时超过阈值，请检查现场是否起火或设备故障"
        if (source === "door" && type === "open") return "检测到门磁打开，请确认是否为授权操作"
        return detail || "请检查设备和现场状态"
    }
    function eventTimeText(value) {
        if (!value) return "时间未知"
        var text = String(value)
        if (text.length >= 19 && text.charAt(10) === "T")
            return text.slice(0, 10) + " " + text.slice(11, 19)
        return text
    }
    function eventMeasurementText(value, threshold) {
        if (value === undefined || value === null || threshold === undefined || threshold === null)
            return ""
        return "数值 " + Number(value).toFixed(1) + " / 阈值 " + Number(threshold).toFixed(1)
    }
    function open(component) { stack.push(component) }

    component Header: Rectangle {
        property string title: ""
        property bool videoPage: false
        Layout.fillWidth: true
        Layout.preferredHeight: 72
        color: window.bg
        ToolButton { anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter; width: 60; height: 56; text: "‹"; font.pixelSize: 38; onClicked: { if (parent.videoPage) videoBridge.setActive(false); stack.pop() } }
        Text { anchors.left: parent.left; anchors.leftMargin: 68; anchors.verticalCenter: parent.verticalCenter; text: parent.title; color: window.text; font.pixelSize: 25; font.bold: true }
    }
    component Panel: Rectangle {
        property string label: ""
        property string value: ""
        property color valueColor: window.text
        color: window.panel; border.color: window.line; border.width: 1; radius: 6
        Column { anchors.fill: parent; anchors.margins: 16; spacing: 8; Text { text: parent.parent.label; color: window.muted; font.pixelSize: 14 } Text { text: parent.parent.value; color: parent.parent.valueColor; font.pixelSize: 21; font.bold: true; elide: Text.ElideRight; width: parent.width } }
    }
    component Action: Button {
        property string caption: ""
        property string glyph: ""
        property color accent: window.ok
        text: ""
        background: Rectangle { color: parent.pressed ? "#2a353c" : window.panel; border.color: window.line; border.width: 1; radius: 6 }
        contentItem: Column { spacing: 12; anchors.centerIn: parent; Text { anchors.horizontalCenter: parent.horizontalCenter; text: parent.parent.glyph; color: parent.parent.accent; font.pixelSize: 30; font.bold: true } Text { anchors.horizontalCenter: parent.horizontalCenter; text: parent.parent.caption; color: window.text; font.pixelSize: 18 } }
    }
    component StatusBar: Rectangle {
        Layout.fillWidth: true; Layout.preferredHeight: 58; color: window.panel; border.color: window.line; border.width: 1; radius: 6
        RowLayout { anchors.fill: parent; anchors.margins: 12; spacing: 12
            Text { text: "摄像头 " + window.stateText(runtime.cameraState, "online"); color: runtime.cameraState === "online" ? window.ok : window.warn; font.pixelSize: 14; Layout.fillWidth: true }
            Text { text: "AI " + window.stateText(runtime.aiState, "running"); color: runtime.aiState === "running" ? window.ok : window.warn; font.pixelSize: 14; Layout.fillWidth: true }
            Text { text: runtime.alarmActive || runtime.roiActive ? "告警活动" : "告警正常"; color: window.statusColor(runtime.alarmActive || runtime.roiActive, runtime.stale); font.pixelSize: 14; Layout.fillWidth: true }
        }
    }

    Component { id: home
        Item { ColumnLayout { anchors.fill: parent; anchors.margins: 22; spacing: 14
            RowLayout { Layout.fillWidth: true; Text { text: "工业安全监控"; color: window.text; font.pixelSize: 29; font.bold: true; Layout.fillWidth: true } Button { text: "返回桌面"; height: 52; onClicked: Qt.quit() } }
            Text { text: runtime.valid && !runtime.stale ? "运行状态：在线" : "运行状态：降级"; color: runtime.valid && !runtime.stale ? window.ok : window.warn; font.pixelSize: 16 }
            GridLayout { Layout.fillWidth: true; Layout.fillHeight: true; columns: 2; rowSpacing: 14; columnSpacing: 14
                Action { Layout.fillWidth: true; Layout.fillHeight: true; caption: "实时监控"; glyph: "●"; accent: "#55b9d8"; onClicked: window.open(monitor) }
                Action { Layout.fillWidth: true; Layout.fillHeight: true; caption: "告警中心"; glyph: "!"; accent: window.danger; onClicked: window.open(alarm) }
                Action { Layout.fillWidth: true; Layout.fillHeight: true; caption: "环境监测"; glyph: "~"; accent: window.ok; onClicked: window.open(environment) }
                Action { Layout.fillWidth: true; Layout.fillHeight: true; caption: "历史记录"; glyph: "≡"; accent: window.warn; onClicked: window.open(historyPage) }
                Action { Layout.fillWidth: true; Layout.fillHeight: true; caption: "系统诊断"; glyph: "i"; accent: "#a6b9c8"; onClicked: window.open(diagnostics) }
                Item { Layout.fillWidth: true; Layout.fillHeight: true }
            }
            StatusBar {}
        } }
    }

    Component { id: monitor
        Item { Component.onCompleted: videoBridge.setActive(true); Component.onDestruction: videoBridge.setActive(false); ColumnLayout { anchors.fill: parent; spacing: 0; Header { title: "实时监控"; videoPage: true }
            Rectangle { id: videoArea; Layout.fillWidth: true; Layout.fillHeight: true; Layout.margins: 20; color: "#070d10"; border.color: "#526b76"; border.width: 1; radius: 4
                Text { anchors.centerIn: parent; visible: videoBridge.state !== "播放中"; text: videoBridge.state + (videoBridge.error.length ? "\n" + videoBridge.error : ""); horizontalAlignment: Text.AlignHCenter; color: window.muted; font.pixelSize: 18 }
                function updateVideoRect() { var point = videoArea.mapToItem(null, 0, 0); videoBridge.setRenderRect(point.x, point.y, width, height) }
                onWidthChanged: updateVideoRect()
                onHeightChanged: updateVideoRect()
                onXChanged: updateVideoRect()
                onYChanged: updateVideoRect()
                Component.onCompleted: updateVideoRect()
            }
            GridLayout { Layout.fillWidth: true; Layout.margins: 20; columns: 2; rowSpacing: 10; columnSpacing: 10
                Panel { Layout.fillWidth: true; Layout.preferredHeight: 88; label: "当前告警"; value: runtime.alarmActive || runtime.roiActive ? runtime.alarmState : "无活动告警"; valueColor: window.statusColor(runtime.alarmActive || runtime.roiActive, runtime.stale) }
                Panel { Layout.fillWidth: true; Layout.preferredHeight: 88; label: "AI 状态"; value: window.stateText(runtime.aiState, "running"); valueColor: runtime.aiState === "running" ? window.ok : window.warn }
                Panel { Layout.fillWidth: true; Layout.preferredHeight: 88; label: "视频"; value: videoBridge.state; valueColor: videoBridge.state === "播放中" ? window.ok : window.warn }
                Panel { Layout.fillWidth: true; Layout.preferredHeight: 88; label: "指标"; value: videoBridge.fps.toFixed(1) + " FPS / " + videoBridge.latencyMs.toFixed(0) + " ms"; valueColor: window.muted }
            }
            RowLayout { Layout.fillWidth: true; Layout.margins: 20; Layout.bottomMargin: 18; Button { text: "重连视频"; height: 52; onClicked: videoBridge.reconnect() } Text { text: "推理帧 " + runtime.framesInferred + "  ·  丢帧 " + videoBridge.dropped; color: window.muted; font.pixelSize: 14; Layout.fillWidth: true; horizontalAlignment: Text.AlignRight } }
        } }
    }

    Component { id: alarm
        Item { Component.onCompleted: { history.reload(20); activeHistory.reload(20) } ColumnLayout { anchors.fill: parent; anchors.margins: 20; spacing: 14; Header { title: "告警中心" }
            Panel { Layout.fillWidth: true; Layout.preferredHeight: 128; label: "当前最高等级"; value: (runtime.alarmActive || runtime.roiActive || activeHistory.count > 0) ? "需要处理" : "正常"; valueColor: window.statusColor(runtime.alarmActive || runtime.roiActive || activeHistory.count > 0, runtime.stale) }
            GridLayout { Layout.fillWidth: true; columns: 2; columnSpacing: 12; rowSpacing: 12
                Panel { Layout.fillWidth: true; Layout.preferredHeight: 100; label: "来源"; value: runtime.lastEventSource }
                Panel { Layout.fillWidth: true; Layout.preferredHeight: 100; label: "ROI 进入 / 清除"; value: runtime.roiEventCount + " / " + runtime.roiClearCount }
                Panel { Layout.fillWidth: true; Layout.preferredHeight: 100; label: "数据库"; value: history.error.length ? "读取失败" : (history.loading ? "读取中" : "已连接"); valueColor: history.error.length ? window.danger : window.ok }
                Panel { Layout.fillWidth: true; Layout.preferredHeight: 100; label: "安全语义"; value: runtime.stale ? "未知" : ((runtime.alarmActive || runtime.roiActive || activeHistory.count > 0) ? "有风险" : "未活动") }
            }
            RowLayout { Layout.fillWidth: true; Text { text: activeHistory.error.length ? "活动事件错误：" + activeHistory.error : "当前活动数据库事件"; color: activeHistory.error.length ? window.danger : window.muted; font.pixelSize: 15; Layout.fillWidth: true } Button { text: "刷新"; height: 48; onClicked: { history.reload(20); activeHistory.reload(20) } } }
            ListView { Layout.fillWidth: true; Layout.preferredHeight: Math.min(300, contentHeight); clip: true; model: activeHistory; spacing: 8; delegate: Rectangle { width: ListView.view.width; height: 112; color: "#292328"; border.color: window.danger; border.width: 2; radius: 5; RowLayout { anchors.fill: parent; anchors.margins: 14; Text { text: "!"; color: window.danger; font.pixelSize: 26; font.bold: true; Layout.preferredWidth: 30; horizontalAlignment: Text.AlignHCenter } ColumnLayout { Layout.fillWidth: true; Text { text: window.eventTitle(source, eventType); color: window.text; font.pixelSize: 18; font.bold: true; elide: Text.ElideRight; Layout.fillWidth: true } Text { text: window.eventDescription(source, eventType, detail); color: window.warn; font.pixelSize: 14; wrapMode: Text.WordWrap; Layout.fillWidth: true } Text { text: window.eventTimeText(time) + "  ·  活动" + (window.eventMeasurementText(value, threshold).length ? "  ·  " + window.eventMeasurementText(value, threshold) : ""); color: window.muted; font.pixelSize: 12; elide: Text.ElideRight; Layout.fillWidth: true } } } } }
            RowLayout { Layout.fillWidth: true; Text { text: history.error.length ? "数据库错误：" + history.error : "最近数据库事件"; color: history.error.length ? window.danger : window.muted; font.pixelSize: 15; Layout.fillWidth: true } }
            ListView { Layout.fillWidth: true; Layout.fillHeight: true; clip: true; model: history; spacing: 8; delegate: Rectangle { width: ListView.view.width; height: 132; color: window.panel; border.color: level === "alarm" ? window.danger : (level === "warning" ? window.warn : window.line); border.width: level === "alarm" ? 2 : 1; radius: 5; RowLayout { anchors.fill: parent; anchors.margins: 14; Text { text: window.levelText(level); color: level === "alarm" ? window.danger : (level === "warning" ? window.warn : window.muted); font.pixelSize: 17; Layout.preferredWidth: 70 } ColumnLayout { Layout.fillWidth: true; Text { text: window.eventTitle(source, eventType); color: window.text; font.pixelSize: 16; font.bold: true; elide: Text.ElideRight; Layout.fillWidth: true } Text { text: "事件 #" + eventId + "  ·  " + window.eventTimeText(time) + "  ·  " + (active ? "活动" : "已恢复"); color: active ? window.danger : window.ok; font.pixelSize: 13; elide: Text.ElideRight; Layout.fillWidth: true } Text { text: window.eventDescription(source, eventType, detail); color: window.muted; font.pixelSize: 12; elide: Text.ElideRight; Layout.fillWidth: true } Text { text: window.eventMeasurementText(value, threshold); color: window.muted; font.pixelSize: 12; visible: text.length > 0 } } } } }
        } }
    }

    Component { id: environment
        Item { ColumnLayout { anchors.fill: parent; anchors.margins: 20; spacing: 14; Header { title: "环境监测" }
            GridLayout { Layout.fillWidth: true; Layout.fillHeight: true; columns: 2; rowSpacing: 14; columnSpacing: 14
                Panel { Layout.fillWidth: true; Layout.fillHeight: true; label: "温度"; value: runtime.temperatureC.toFixed(1) + " °C" }
                Panel { Layout.fillWidth: true; Layout.fillHeight: true; label: "湿度"; value: runtime.humidityRh.toFixed(1) + " %RH" }
                Panel { Layout.fillWidth: true; Layout.fillHeight: true; label: "光照"; value: runtime.illuminanceLux.toFixed(1) + " lux" }
                Panel { Layout.fillWidth: true; Layout.fillHeight: true; label: "雷达"; value: runtime.radarState; valueColor: runtime.radarState === "online" ? window.ok : window.warn }
            }
        } }
    }

    Component { id: historyPage
        Item { ColumnLayout { anchors.fill: parent; anchors.margins: 20; spacing: 12; Header { title: "历史记录" }
            RowLayout { Layout.fillWidth: true; Text { text: history.error.length ? "历史服务：" + history.error : "最近告警"; color: history.error.length ? window.warn : window.muted; font.pixelSize: 15; Layout.fillWidth: true } Button { text: "刷新"; height: 48; onClicked: history.reload() } }
            ListView { Layout.fillWidth: true; Layout.fillHeight: true; clip: true; model: history; spacing: 8; delegate: Rectangle { width: ListView.view.width; height: 132; color: window.panel; border.color: level === "alarm" ? window.danger : (level === "warning" ? window.warn : window.line); border.width: level === "alarm" ? 2 : 1; radius: 5; RowLayout { anchors.fill: parent; anchors.margins: 14; Text { text: window.levelText(level); color: level === "alarm" ? window.danger : window.warn; font.pixelSize: 16; Layout.preferredWidth: 70 } ColumnLayout { Layout.fillWidth: true; Text { text: window.eventTitle(source, eventType); color: window.text; font.pixelSize: 16; font.bold: true; elide: Text.ElideRight; Layout.fillWidth: true } Text { text: "事件 #" + eventId + "  ·  " + window.eventTimeText(time) + "  ·  " + (active ? "活动" : "已恢复"); color: active ? window.danger : window.ok; font.pixelSize: 12; elide: Text.ElideRight; Layout.fillWidth: true } Text { text: window.eventDescription(source, eventType, detail); color: window.muted; font.pixelSize: 12; wrapMode: Text.WordWrap; Layout.fillWidth: true } Text { text: window.eventMeasurementText(value, threshold); color: window.muted; font.pixelSize: 12; visible: text.length > 0 } } } } }
        } }
    }

    Component { id: diagnostics
        Item { ColumnLayout { anchors.fill: parent; anchors.margins: 20; spacing: 12; Header { title: "系统诊断 / 设置" }
            Panel { Layout.fillWidth: true; Layout.preferredHeight: 96; label: "状态服务"; value: runtime.valid ? (runtime.stale ? "数据过期" : "在线") : runtime.error; valueColor: runtime.valid && !runtime.stale ? window.ok : window.warn }
            Panel { Layout.fillWidth: true; Layout.preferredHeight: 96; label: "视频链路"; value: videoBridge.state; valueColor: videoBridge.state === "播放中" ? window.ok : window.warn }
            Panel { Layout.fillWidth: true; Layout.preferredHeight: 96; label: "状态文件"; value: runtime.statePath; valueColor: window.muted }
            Panel { Layout.fillWidth: true; Layout.preferredHeight: 96; label: "运行指标"; value: "预览 " + runtime.previewSaved + " / " + runtime.previewFailed + "，推理 " + runtime.framesInferred; valueColor: window.muted }
            Text { text: "参数修改、告警确认和服务重启接口尚未开放"; color: window.warn; font.pixelSize: 15; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            Item { Layout.fillHeight: true }
        } }
    }

    StackView { id: stack; anchors.fill: parent; initialItem: home; replaceEnter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 120 } } }
}
