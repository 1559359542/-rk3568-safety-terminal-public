#include "runtime_model.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <utility>

namespace {
QString stringValue(const QJsonObject &object, const char *key, const QString &fallback)
{
    const QJsonValue value = object.value(QLatin1String(key));
    return value.isString() ? value.toString() : fallback;
}

qulonglong integerValue(const QJsonObject &object, const char *key)
{
    const QJsonValue value = object.value(QLatin1String(key));
    return value.isDouble() ? static_cast<qulonglong>(value.toDouble()) : 0;
}

double numberValue(const QJsonObject &object, const char *key)
{
    const QJsonValue value = object.value(QLatin1String(key));
    return value.isDouble() ? value.toDouble() : 0.0;
}
}

RuntimeModel::RuntimeModel(QString statePath, QObject *parent)
    : QObject(parent), statePath_(std::move(statePath))
{
    connect(&timer_, &QTimer::timeout, this, &RuntimeModel::refresh);
    timer_.start(500);
    refresh();
}

void RuntimeModel::refresh()
{
    QFile file(statePath_);
    if (!file.open(QIODevice::ReadOnly)) {
        valid_ = false;
        stale_ = true;
        error_ = QStringLiteral("状态文件缺失或不可读");
        emit changed();
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        valid_ = false;
        stale_ = true;
        error_ = QStringLiteral("状态文件格式错误");
        emit changed();
        return;
    }

    const QJsonObject root = document.object();
    const QJsonObject alarm = root.value(QStringLiteral("alarm")).toObject();
    const QJsonObject radar = root.value(QStringLiteral("ld2410")).toObject();
    const QJsonObject sht3x = root.value(QStringLiteral("sht3x")).toObject();
    const QJsonObject bh1750 = root.value(QStringLiteral("bh1750")).toObject();

    valid_ = true;
    error_.clear();
    cameraState_ = stringValue(root, "camera_state", "unknown");
    aiState_ = stringValue(root, "ai_state", "unknown");
    alarmState_ = stringValue(alarm, "state", "unknown");
    alarmActive_ = alarm.value(QStringLiteral("ai_alarm")).toBool() ||
                   alarmState_ == QStringLiteral("active");
    roiActive_ = root.value(QStringLiteral("roi_active")).toBool();
    framesInferred_ = integerValue(root, "frames_inferred");
    previewSaved_ = integerValue(root, "preview_saved");
    previewFailed_ = integerValue(root, "preview_failed");
    roiEventCount_ = integerValue(root, "roi_event_count");
    roiClearCount_ = integerValue(root, "roi_clear_count");
    previewTransport_ = stringValue(root, "preview_transport", "unavailable");
    previewStreamState_ = stringValue(root, "preview_stream_state", "unavailable");
    lastEventSource_ = stringValue(root, "last_event_source", "none");
    radarState_ = stringValue(radar, "state", "unknown");
    temperatureC_ = numberValue(sht3x, "temperature_c");
    humidityRh_ = numberValue(sht3x, "humidity_rh");
    illuminanceLux_ = numberValue(bh1750, "value_lux");
    previewFps_ = numberValue(root, "preview_fps");
    previewLatencyMs_ = numberValue(root, "preview_latency_ms");

    const qint64 monotonicMs = static_cast<qint64>(numberValue(root, "monotonic_ms"));
    // A monotonic timestamp cannot be compared to wall-clock time. Treat a
    // missing/zero timestamp as stale and leave freshness to the publisher.
    stale_ = monotonicMs <= 0;
    emit changed();
}
