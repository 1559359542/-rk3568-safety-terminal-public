#include "monitor_window.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLabel>
#include <QResizeEvent>
#include <QShowEvent>
#include <QSocketNotifier>
#include <QTimer>
#include <QVBoxLayout>

#include <time.h>
#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/video/videooverlay.h>

namespace {

constexpr int kRefreshIntervalMs = 200;
constexpr qint64 kStateStaleAfterMs = 3000;
constexpr qint64 kPreviewStaleAfterMs = 3000;
constexpr quint32 kH264Magic = 0x524b4832U;
constexpr quint32 kH264Config = 1U;
constexpr quint32 kH264AccessUnit = 2U;
constexpr quint32 kH264Idr = 1U << 1;

struct H264SocketHeader {
    quint32 magic;
    quint32 type;
    quint32 flags;
    quint32 payloadSize;
    quint64 sequence;
    quint64 capturedAtMs;
};

QLabel *makeLabel(const QString &text, int pointSize, const QString &style = {})
{
    auto *label = new QLabel(text);
    QFont font = label->font();
    font.setPointSize(pointSize);
    label->setFont(font);
    label->setWordWrap(true);
    if (!style.isEmpty()) {
        label->setStyleSheet(style);
    }
    return label;
}

QFrame *makePanel()
{
    auto *panel = new QFrame();
    panel->setFrameShape(QFrame::StyledPanel);
    panel->setStyleSheet(
        "QFrame { background: #192027; border: 1px solid #37424c; border-radius: 4px; }");
    return panel;
}

bool requiredString(const QJsonObject &object, const char *key, QString *value)
{
    const QJsonValue jsonValue = object.value(QLatin1String(key));
    if (!jsonValue.isString()) {
        return false;
    }
    *value = jsonValue.toString();
    return !value->isEmpty();
}

bool requiredUnsigned(const QJsonObject &object, const char *key, quint64 *value)
{
    const QJsonValue jsonValue = object.value(QLatin1String(key));
    if (!jsonValue.isDouble() || jsonValue.toDouble() < 0.0) {
        return false;
    }
    *value = static_cast<quint64>(jsonValue.toDouble());
    return true;
}

} // namespace

MonitorWindow::MonitorWindow(QString runtimeDir, QWidget *parent)
    : QWidget(parent), runtimeDir_(std::move(runtimeDir))
{
    setAttribute(Qt::WA_NativeWindow);
    gst_init(nullptr, nullptr);
    setWindowTitle(QStringLiteral("RK3568 工业安全监测"));
    setStyleSheet("QWidget { background: #101419; color: #edf2f7; }");

    auto *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(18, 16, 18, 16);
    rootLayout->setSpacing(12);

    auto *header = new QHBoxLayout();
    auto *title = makeLabel(QStringLiteral("工业安全监测"), 20, "font-weight: 600;");
    summaryLabel_ = makeLabel(QStringLiteral("状态同步中"), 13, "color: #aab7c4;");
    summaryLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    header->addWidget(title, 1);
    header->addWidget(summaryLabel_);
    rootLayout->addLayout(header);

    auto *previewPanel = makePanel();
    auto *previewLayout = new QVBoxLayout(previewPanel);
    previewLayout->setContentsMargins(6, 6, 6, 6);
    previewLabel_ = makeLabel(QStringLiteral("等待 B1 预览帧"), 17, "color: #aab7c4;");
    previewLabel_->setAlignment(Qt::AlignCenter);
    previewLabel_->setMinimumHeight(610);
    previewLabel_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    previewOverlay_ = makeLabel(QString(), 13, "color: #ffcf70; background: transparent;");
    previewOverlay_->setAlignment(Qt::AlignCenter);
    previewLayout->addWidget(previewLabel_);
    previewLayout->addWidget(previewOverlay_);
    rootLayout->addWidget(previewPanel, 1);

    auto *statusGrid = new QGridLayout();
    statusGrid->setHorizontalSpacing(10);
    statusGrid->setVerticalSpacing(10);

    auto *cameraPanel = makePanel();
    auto *cameraLayout = new QVBoxLayout(cameraPanel);
    cameraLayout->addWidget(makeLabel(QStringLiteral("摄像头"), 12, "color: #aab7c4;"));
    cameraLabel_ = makeLabel(QStringLiteral("未知"), 16);
    cameraLayout->addWidget(cameraLabel_);

    auto *aiPanel = makePanel();
    auto *aiLayout = new QVBoxLayout(aiPanel);
    aiLayout->addWidget(makeLabel(QStringLiteral("AI 推理"), 12, "color: #aab7c4;"));
    aiLabel_ = makeLabel(QStringLiteral("未知"), 16);
    aiLayout->addWidget(aiLabel_);

    auto *alarmPanel = makePanel();
    auto *alarmLayout = new QVBoxLayout(alarmPanel);
    alarmLayout->addWidget(makeLabel(QStringLiteral("ROI 告警"), 12, "color: #aab7c4;"));
    alarmLabel_ = makeLabel(QStringLiteral("状态未知"), 16);
    alarmLayout->addWidget(alarmLabel_);

    auto *countPanel = makePanel();
    auto *countLayout = new QVBoxLayout(countPanel);
    countLayout->addWidget(makeLabel(QStringLiteral("关键计数"), 12, "color: #aab7c4;"));
    countsLabel_ = makeLabel(QStringLiteral("等待状态快照"), 13);
    countLayout->addWidget(countsLabel_);

    statusGrid->addWidget(cameraPanel, 0, 0);
    statusGrid->addWidget(aiPanel, 0, 1);
    statusGrid->addWidget(alarmPanel, 1, 0);
    statusGrid->addWidget(countPanel, 1, 1);
    rootLayout->addLayout(statusGrid);

    diagnosticLabel_ = makeLabel(QStringLiteral("只读输入：system_state.json 与 latest.jpg"), 11,
                                 "color: #aab7c4;");
    rootLayout->addWidget(diagnosticLabel_);

    refreshTimer_ = new QTimer(this);
    connect(refreshTimer_, &QTimer::timeout, this, &MonitorWindow::refresh);
    refreshTimer_->start(kRefreshIntervalMs);
    gstreamerBusTimer_ = new QTimer(this);
    connect(gstreamerBusTimer_, &QTimer::timeout, this, &MonitorWindow::pollGStreamerBus);
    gstreamerBusTimer_->start(50);
    refresh();
}

MonitorWindow::~MonitorWindow()
{
    disconnectH264Socket(QStringLiteral("关闭"));
    stopGStreamerPipeline();
}

MonitorWindow::RuntimeSnapshot MonitorWindow::loadSnapshot() const
{
    RuntimeSnapshot snapshot;
    QFile file(QStringLiteral("/run/industrial_safety/system_state.json"));
    if (!file.open(QIODevice::ReadOnly)) {
        snapshot.error = QStringLiteral("状态文件缺失或不可读");
        return snapshot;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        snapshot.error = QStringLiteral("状态文件格式错误");
        return snapshot;
    }

    const QJsonObject object = document.object();
    const QJsonValue schemaVersion = object.value(QStringLiteral("schema_version"));
    quint64 monotonic = 0;
    if (!schemaVersion.isDouble() || schemaVersion.toInt() != 1 ||
        !requiredUnsigned(object, "monotonic_ms", &monotonic) ||
        !requiredString(object, "camera_state", &snapshot.cameraState) ||
        !requiredString(object, "ai_state", &snapshot.aiState) ||
        !requiredString(object, "preview_frame_path", &snapshot.previewPath) ||
        !requiredUnsigned(object, "preview_frame_sequence", &snapshot.previewSequence) ||
        !requiredUnsigned(object, "frames_inferred", &snapshot.framesInferred) ||
        !requiredUnsigned(object, "preview_saved", &snapshot.previewSaved) ||
        !requiredUnsigned(object, "preview_failed", &snapshot.previewFailed) ||
        !requiredUnsigned(object, "roi_event_count", &snapshot.roiEventCount) ||
        !requiredUnsigned(object, "roi_clear_count", &snapshot.roiClearCount) ||
        !requiredString(object, "last_event_source", &snapshot.lastEventSource) ||
        !object.value(QStringLiteral("roi_active")).isBool()) {
        snapshot.error = QStringLiteral("状态字段缺失或类型错误");
        return snapshot;
    }

    snapshot.monotonicMs = static_cast<qint64>(monotonic);
    snapshot.roiActive = object.value(QStringLiteral("roi_active")).toBool();
    snapshot.lastEventMonotonicMs = static_cast<qint64>(
        object.value(QStringLiteral("last_event_monotonic_ms")).toDouble());
    snapshot.previewTransport = object.value(QStringLiteral("preview_transport")).toString();
    snapshot.previewStreamState = object.value(QStringLiteral("preview_stream_state")).toString();
    snapshot.previewFps = object.value(QStringLiteral("preview_fps")).toDouble();
    snapshot.previewLatencyMs = object.value(QStringLiteral("preview_latency_ms")).toDouble();
    const QJsonObject alarm = object.value(QStringLiteral("alarm")).toObject();
    const QJsonObject ld2410 = object.value(QStringLiteral("ld2410")).toObject();
    const QJsonObject sht3x = object.value(QStringLiteral("sht3x")).toObject();
    const QJsonObject bh1750 = object.value(QStringLiteral("bh1750")).toObject();
    if (!requiredString(alarm, "state", &snapshot.alarmState) ||
        !alarm.value(QStringLiteral("applied_alarm")).isBool() ||
        !alarm.value(QStringLiteral("radar_alarm")).isBool() ||
        !alarm.value(QStringLiteral("ai_alarm")).isBool() ||
        !requiredString(ld2410, "state", &snapshot.radarState) ||
        !requiredString(sht3x, "state", &snapshot.sht3xState) ||
        !requiredString(bh1750, "state", &snapshot.bh1750State)) {
        snapshot.error = QStringLiteral("统一状态字段缺失或类型错误");
        return snapshot;
    }

    snapshot.radarTargetState = ld2410.value(QStringLiteral("radar_state")).toString();
    snapshot.radarCalibration = ld2410.value(QStringLiteral("calibration")).toString();
    quint64 detectCm = 0;
    quint64 moveCm = 0;
    quint64 moveEnergy = 0;
    snapshot.radarMetricsValid =
        requiredUnsigned(ld2410, "detect_cm", &detectCm) &&
        requiredUnsigned(ld2410, "move_cm", &moveCm) &&
        requiredUnsigned(ld2410, "move_energy", &moveEnergy);
    if (snapshot.radarMetricsValid) {
        snapshot.radarDetectCm = detectCm;
        snapshot.radarMoveCm = moveCm;
        snapshot.radarMoveEnergy = moveEnergy;
    }

    const QJsonValue temperature = sht3x.value(QStringLiteral("temperature_c"));
    const QJsonValue humidity = sht3x.value(QStringLiteral("humidity_rh"));
    snapshot.sht3xMetricsValid = temperature.isDouble() && humidity.isDouble();
    if (snapshot.sht3xMetricsValid) {
        snapshot.temperatureC = temperature.toDouble();
        snapshot.humidityRh = humidity.toDouble();
    }

    const QJsonValue lux = bh1750.value(QStringLiteral("value_lux"));
    snapshot.bh1750LuxValid = lux.isDouble();
    if (snapshot.bh1750LuxValid) {
        snapshot.illuminanceLux = lux.toDouble();
    }
    snapshot.mergedAlarmActive = alarm.value(QStringLiteral("applied_alarm")).toBool() ||
                                 alarm.value(QStringLiteral("radar_alarm")).toBool() ||
                                 alarm.value(QStringLiteral("ai_alarm")).toBool();
    snapshot.valid = true;
    return snapshot;
}

bool MonitorWindow::loadPreview(const RuntimeSnapshot &snapshot)
{
    QPixmap candidate(resolvePreviewPath(snapshot.previewPath));
    if (candidate.isNull()) {
        preview_ = QPixmap();
        scaledPreview_ = QPixmap();
        scaledPreviewSize_ = QSize();
        return false;
    }
    preview_ = candidate;
    scaledPreview_ = QPixmap();
    scaledPreviewSize_ = QSize();
    return true;
}

bool MonitorWindow::startGStreamerPipeline()
{
    if (gstPipeline_ != nullptr) {
        return true;
    }
    GError *error = nullptr;
    gstPipeline_ = GST_ELEMENT(gst_parse_launch(
        "appsrc name=src is-live=true format=time do-timestamp=true block=false "
        "! h264parse config-interval=-1 ! mppvideodec ! waylandsink name=sink sync=false",
        &error));
    if (gstPipeline_ == nullptr) {
        h264Status_ = QStringLiteral("unavailable");
        if (error != nullptr) {
            g_error_free(error);
        }
        return false;
    }
    gstAppsrc_ = gst_bin_get_by_name(GST_BIN(gstPipeline_), "src");
    gstSink_ = gst_bin_get_by_name(GST_BIN(gstPipeline_), "sink");
    if (gstAppsrc_ == nullptr || gstSink_ == nullptr) {
        if (gstSink_ != nullptr) {
            gst_object_unref(gstSink_);
            gstSink_ = nullptr;
        }
        stopGStreamerPipeline();
        h264Status_ = QStringLiteral("unavailable");
        return false;
    }
    gstBus_ = gst_element_get_bus(gstPipeline_);
    gst_video_overlay_set_window_handle(
        GST_VIDEO_OVERLAY(gstSink_), static_cast<guintptr>(winId()));
    updateGStreamerRenderRectangle();
    if (gst_element_set_state(gstPipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        stopGStreamerPipeline();
        h264Status_ = QStringLiteral("unavailable");
        return false;
    }
    h264Active_ = true;
    h264Status_ = QStringLiteral("jpeg_fallback");
    return true;
}

void MonitorWindow::stopGStreamerPipeline()
{
    if (gstPipeline_ != nullptr) {
        gst_element_set_state(gstPipeline_, GST_STATE_NULL);
    }
    if (gstBus_ != nullptr) {
        gst_object_unref(gstBus_);
        gstBus_ = nullptr;
    }
    if (gstAppsrc_ != nullptr) {
        gst_object_unref(gstAppsrc_);
        gstAppsrc_ = nullptr;
    }
    if (gstSink_ != nullptr) {
        gst_object_unref(gstSink_);
        gstSink_ = nullptr;
    }
    if (gstPipeline_ != nullptr) {
        gst_object_unref(gstPipeline_);
        gstPipeline_ = nullptr;
    }
    h264Active_ = false;
}

void MonitorWindow::setH264Fallback(const QString &reason)
{
    h264Status_ = reason.isEmpty() ? QStringLiteral("jpeg_fallback") : reason;
    h264ConfigReceived_ = false;
    h264WaitingForIdr_ = true;
}

void MonitorWindow::connectH264Socket()
{
    if (!h264Active_ || h264Fd_ >= 0) {
        return;
    }
    const QString path = QDir(runtimeDir_).filePath(QStringLiteral("h264.sock"));
    const int fd = ::socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (fd < 0) {
        setH264Fallback(QStringLiteral("jpeg_fallback"));
        return;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const QByteArray encoded = QFile::encodeName(path);
    if (encoded.size() >= static_cast<int>(sizeof(address.sun_path))) {
        ::close(fd);
        setH264Fallback(QStringLiteral("unavailable"));
        return;
    }
    memcpy(address.sun_path, encoded.constData(), static_cast<size_t>(encoded.size()) + 1);
    if (::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        ::close(fd);
        setH264Fallback(QStringLiteral("jpeg_fallback"));
        return;
    }
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        ::close(fd);
        setH264Fallback(QStringLiteral("unavailable"));
        return;
    }
    h264Fd_ = fd;
    h264ConfigReceived_ = false;
    h264WaitingForIdr_ = true;
    h264Notifier_ = new QSocketNotifier(h264Fd_, QSocketNotifier::Read, this);
    connect(h264Notifier_, &QSocketNotifier::activated,
            this, [this](int) { drainH264Socket(); });
    h264Status_ = QStringLiteral("jpeg_fallback");
}

void MonitorWindow::disconnectH264Socket(const QString &reason)
{
    if (h264Notifier_ != nullptr) {
        h264Notifier_->setEnabled(false);
        h264Notifier_->deleteLater();
        h264Notifier_ = nullptr;
    }
    if (h264Fd_ >= 0) {
        ::close(h264Fd_);
        h264Fd_ = -1;
    }
    h264ConfigReceived_ = false;
    h264WaitingForIdr_ = true;
    setH264Fallback(reason);
}

void MonitorWindow::drainH264Socket()
{
    if (h264Fd_ < 0 || gstAppsrc_ == nullptr) {
        return;
    }
    QByteArray packet;
    packet.resize(2 * 1024 * 1024);
    const ssize_t received = ::recv(h264Fd_, packet.data(), packet.size(), MSG_DONTWAIT);
    if (received <= 0) {
        if (received == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            disconnectH264Socket(QStringLiteral("jpeg_fallback"));
        }
        return;
    }
    if (static_cast<size_t>(received) < sizeof(H264SocketHeader)) {
        disconnectH264Socket(QStringLiteral("unavailable"));
        return;
    }
    H264SocketHeader header{};
    memcpy(&header, packet.constData(), sizeof(header));
    if (header.magic != kH264Magic ||
        header.payloadSize != static_cast<quint32>(received - sizeof(header))) {
        disconnectH264Socket(QStringLiteral("unavailable"));
        return;
    }
    const QByteArray payload(packet.constData() + sizeof(header),
                             static_cast<int>(header.payloadSize));
    if (header.type == kH264Config) {
        h264ConfigReceived_ = true;
        h264WaitingForIdr_ = true;
    } else if (header.type == kH264AccessUnit) {
        if (!h264ConfigReceived_ ||
            (h264WaitingForIdr_ && (header.flags & kH264Idr) == 0)) {
            return;
        }
        if ((header.flags & kH264Idr) != 0) {
            h264WaitingForIdr_ = false;
        }
    } else {
        return;
    }
    GstBuffer *buffer = gst_buffer_new_allocate(nullptr, payload.size(), nullptr);
    if (buffer == nullptr) {
        disconnectH264Socket(QStringLiteral("jpeg_fallback"));
        return;
    }
    gst_buffer_fill(buffer, 0, payload.constData(), payload.size());
    const GstFlowReturn result = gst_app_src_push_buffer(GST_APP_SRC(gstAppsrc_), buffer);
    if (result != GST_FLOW_OK) {
        disconnectH264Socket(QStringLiteral("jpeg_fallback"));
        return;
    }
    h264Status_ = QStringLiteral("h264");
}

void MonitorWindow::pollGStreamerBus()
{
    if (gstBus_ == nullptr) {
        connectH264Socket();
        return;
    }
    for (;;) {
        GstMessage *message = gst_bus_pop_filtered(
            gstBus_, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
        if (message == nullptr) {
            break;
        }
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
            disconnectH264Socket(QStringLiteral("jpeg_fallback"));
        }
        gst_message_unref(message);
    }
    connectH264Socket();
}

void MonitorWindow::updatePreviewPixmap()
{
    if (preview_.isNull()) {
        return;
    }

    const QSize targetSize = previewLabel_->contentsRect().size();
    if (targetSize.isEmpty() ||
        (targetSize == scaledPreviewSize_ && !scaledPreview_.isNull())) {
        return;
    }

    scaledPreview_ = preview_.scaled(targetSize, Qt::KeepAspectRatio,
                                     Qt::SmoothTransformation);
    scaledPreviewSize_ = targetSize;
    previewLabel_->setPixmap(scaledPreview_);
}

void MonitorWindow::refresh()
{
    connectH264Socket();
    const RuntimeSnapshot snapshot = loadSnapshot();
    if (!snapshot.valid) {
        previewOverlay_->setText(QStringLiteral("预览不可用：状态未就绪"));
        updateStatus(snapshot, true, true);
        return;
    }

    if (!hasSequence_ || snapshot.previewSequence != lastSequence_) {
        hasSequence_ = true;
        lastSequence_ = snapshot.previewSequence;
        sequenceClock_.restart();
        if (!loadPreview(snapshot)) {
            previewOverlay_->setText(QStringLiteral("预览不可用：JPEG 加载失败"));
            previewLabel_->setPixmap(QPixmap());
            previewLabel_->setText(QStringLiteral("预览不可用：JPEG 加载失败"));
        }
    }

    const qint64 age = monotonicMs() - snapshot.monotonicMs;
    const bool stateStale = age < 0 || age > kStateStaleAfterMs;
    const bool previewStale = !hasSequence_ || !sequenceClock_.isValid() ||
        sequenceClock_.elapsed() > kPreviewStaleAfterMs || preview_.isNull();

    updatePreviewPixmap();
    if (previewStale) {
        previewOverlay_->setText(QStringLiteral("预览已过期"));
    } else if (h264Status_ == QStringLiteral("unavailable")) {
        previewOverlay_->setText(QStringLiteral("unavailable"));
    } else if (h264Status_ != QStringLiteral("h264")) {
        previewOverlay_->setText(QStringLiteral("jpeg_fallback"));
    } else {
        previewOverlay_->setText(QString());
    }
    updateStatus(snapshot, stateStale, previewStale);
}

void MonitorWindow::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    updatePreviewPixmap();
    updateGStreamerRenderRectangle();
}

void MonitorWindow::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (h264StartupScheduled_) {
        return;
    }
    h264StartupScheduled_ = true;
    QTimer::singleShot(0, this, [this]() {
        if (!startGStreamerPipeline()) {
            h264Status_ = QStringLiteral("unavailable");
        }
        refresh();
    });
}

void MonitorWindow::updateGStreamerRenderRectangle()
{
    if (gstSink_ == nullptr || !GST_IS_VIDEO_OVERLAY(gstSink_) || previewLabel_ == nullptr) {
        return;
    }
    const QRect rect = previewLabel_->contentsRect();
    const QPoint topLeft = previewLabel_->mapTo(this, rect.topLeft());
    gst_video_overlay_set_render_rectangle(GST_VIDEO_OVERLAY(gstSink_),
                                            topLeft.x(), topLeft.y(),
                                            rect.width(), rect.height());
}

void MonitorWindow::updateStatus(const RuntimeSnapshot &snapshot, bool stateStale, bool previewStale)
{
    if (!snapshot.valid) {
        summaryLabel_->setText(QStringLiteral("状态不可用"));
        summaryLabel_->setStyleSheet("color: #ff8d8d;");
        cameraLabel_->setText(QStringLiteral("未知"));
        aiLabel_->setText(QStringLiteral("未知"));
        alarmLabel_->setText(QStringLiteral("未知，不显示安全"));
        countsLabel_->setText(snapshot.error);
        diagnosticLabel_->setText(QStringLiteral("降级原因：%1").arg(snapshot.error));
        return;
    }

    const bool cameraOnline = snapshot.cameraState == QStringLiteral("online");
    const bool aiRunning = snapshot.aiState == QStringLiteral("running");
    const bool degraded = stateStale || previewStale || !cameraOnline || !aiRunning;

    summaryLabel_->setText(degraded ? QStringLiteral("降级运行") : QStringLiteral("运行正常"));
    summaryLabel_->setStyleSheet(degraded ? "color: #ffcf70;" : "color: #77d69a;");
    cameraLabel_->setText(stateText(snapshot.cameraState, QStringLiteral("在线"), QStringLiteral("不可用")));
    aiLabel_->setText(stateText(snapshot.aiState, QStringLiteral("运行中"), QStringLiteral("不可用")));

    if (snapshot.mergedAlarmActive || snapshot.roiActive) {
        alarmLabel_->setText(QStringLiteral("告警活动"));
        alarmLabel_->setStyleSheet("color: #ff8d8d; font-weight: 600;");
    } else if (stateStale || !aiRunning) {
        alarmLabel_->setText(QStringLiteral("未知，不显示安全"));
        alarmLabel_->setStyleSheet("color: #ffcf70;");
    } else {
        alarmLabel_->setText(QStringLiteral("未活动"));
        alarmLabel_->setStyleSheet("color: #77d69a;");
    }

    const QString radarTarget = snapshot.radarTargetState.isEmpty()
        ? QStringLiteral("未知") : snapshot.radarTargetState;
    const QString radarCalibration = snapshot.radarCalibration.isEmpty()
        ? QStringLiteral("未知") : snapshot.radarCalibration;
    const QString detectDistance = snapshot.radarMetricsValid
        ? QString::number(snapshot.radarDetectCm) : QStringLiteral("未知");
    const QString moveDistance = snapshot.radarMetricsValid
        ? QString::number(snapshot.radarMoveCm) : QStringLiteral("未知");
    const QString moveEnergy = snapshot.radarMetricsValid
        ? QString::number(snapshot.radarMoveEnergy) : QStringLiteral("未知");
    const QString temperature = snapshot.sht3xMetricsValid
        ? QString::number(snapshot.temperatureC, 'f', 1) : QStringLiteral("未知");
    const QString humidity = snapshot.sht3xMetricsValid
        ? QString::number(snapshot.humidityRh, 'f', 1) : QStringLiteral("未知");
    const QString illuminance = snapshot.bh1750LuxValid
        ? QString::number(snapshot.illuminanceLux, 'f', 1) : QStringLiteral("未知");

    countsLabel_->setText(QStringLiteral("推理 %1\n预览成功/失败 %2 / %3\nROI 进入/清除 %4 / %5\n雷达 %6，目标 %7\n探测/移动 %8 / %9 cm，能量 %10\n校准 %11\n温度/湿度 %12 C / %13 RH（%14）\n光照 %15 lux（%16）")
                              .arg(snapshot.framesInferred)
                              .arg(snapshot.previewSaved)
                              .arg(snapshot.previewFailed)
                              .arg(snapshot.roiEventCount)
                              .arg(snapshot.roiClearCount)
                              .arg(snapshot.radarState.isEmpty() ? QStringLiteral("未知") : snapshot.radarState)
                              .arg(radarTarget)
                              .arg(detectDistance)
                              .arg(moveDistance)
                              .arg(moveEnergy)
                              .arg(radarCalibration)
                              .arg(temperature)
                              .arg(humidity)
                              .arg(snapshot.sht3xState)
                              .arg(illuminance)
                              .arg(snapshot.bh1750State));
    const QString transport = snapshot.previewTransport.isEmpty()
        ? h264Status_ : snapshot.previewTransport;
    diagnosticLabel_->setText(QStringLiteral("frame=%1  event=%2  状态%3  预览%4  视频=%5 %6fps %7ms")
                                  .arg(snapshot.previewSequence)
                                  .arg(snapshot.lastEventSource)
                                  .arg(stateStale ? QStringLiteral("过期") : QStringLiteral("新鲜"))
                                  .arg(previewStale ? QStringLiteral("过期") : QStringLiteral("新鲜"))
                                  .arg(transport)
                                  .arg(snapshot.previewFps, 0, 'f', 1)
                                  .arg(snapshot.previewLatencyMs, 0, 'f', 1));
}

QString MonitorWindow::resolvePreviewPath(const QString &publishedPath) const
{
    if (QDir::isAbsolutePath(publishedPath)) {
        return publishedPath;
    }
    // B1 publishes "hmi_runtime/..." relative to the AI working directory.
    QDir publisherBase(runtimeDir_);
    publisherBase.cdUp();
    return publisherBase.filePath(publishedPath);
}

qint64 MonitorWindow::monotonicMs()
{
    struct timespec value {};
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
        return 0;
    }
    return static_cast<qint64>(value.tv_sec) * 1000 + value.tv_nsec / 1000000;
}

QString MonitorWindow::stateText(const QString &state, const QString &onlineText,
                                 const QString &offlineText)
{
    return state == QStringLiteral("online") || state == QStringLiteral("running")
        ? onlineText
        : QStringLiteral("%1：%2").arg(offlineText, state);
}
