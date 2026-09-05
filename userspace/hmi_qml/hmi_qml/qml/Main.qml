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
    function open(component) { stack.push(component) }

    component Header: Rectangle {
        property string title: ""
        Layout.fillWidth: true
        Layout.preferredHeight: 72
        color: window.bg
        ToolButton { anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter; width: 60; height: 56; text: "‹"; font.pixelSize: 38; onClicked: stack.pop() }
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
        Item { ColumnLayout { anchors.fill: parent; spacing: 0; Header { title: "实时监控" }
            Rectangle { id: videoArea; Layout.fillWidth: true; Layout.fillHeight: true; Layout.margins: 20; color: "#070d10"; border.color: "#526b76"; border.width: 1; radius: 4
                Text { anchors.centerIn: parent; visible: videoBridge.state !== "播放中"; text: videoBridge.state + (videoBridge.error.length ? "\n" + videoBridge.error : ""); horizontalAlignment: Text.AlignHCenter; color: window.muted; font.pixelSize: 18 }
                onWidthChanged: videoBridge.setRenderRect(x, y, width, height)
                onHeightChanged: videoBridge.setRenderRect(x, y, width, height)
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
        Item { ColumnLayout { anchors.fill: parent; anchors.margins: 20; spacing: 14; Header { title: "告警中心" }
            Panel { Layout.fillWidth: true; Layout.preferredHeight: 128; label: "当前最高等级"; value: runtime.alarmActive || runtime.roiActive ? runtime.alarmState : "正常"; valueColor: window.statusColor(runtime.alarmActive || runtime.roiActive, runtime.stale) }
            GridLayout { Layout.fillWidth: true; columns: 2; columnSpacing: 12; rowSpacing: 12
                Panel { Layout.fillWidth: true; Layout.preferredHeight: 100; label: "来源"; value: runtime.lastEventSource }
                Panel { Layout.fillWidth: true; Layout.preferredHeight: 100; label: "ROI 进入 / 清除"; value: runtime.roiEventCount + " / " + runtime.roiClearCount }
                Panel { Layout.fillWidth: true; Layout.preferredHeight: 100; label: "确认"; value: "接口未提供"; valueColor: window.warn }
                Panel { Layout.fillWidth: true; Layout.preferredHeight: 100; label: "安全语义"; value: runtime.stale ? "未知" : (runtime.alarmActive ? "有风险" : "未活动") }
            }
            Item { Layout.fillHeight: true }
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
            ListView { Layout.fillWidth: true; Layout.fillHeight: true; clip: true; model: history; spacing: 8; delegate: Rectangle { width: ListView.view.width; height: 82; color: window.panel; border.color: window.line; radius: 5; RowLayout { anchors.fill: parent; anchors.margins: 14; Text { text: level; color: window.danger; font.pixelSize: 17; Layout.preferredWidth: 90 } ColumnLayout { Layout.fillWidth: true; Text { text: source; color: window.text; font.pixelSize: 16 } Text { text: time + "  " + (recovered ? "已恢复" : "活动"); color: window.muted; font.pixelSize: 13 } } } } }
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
