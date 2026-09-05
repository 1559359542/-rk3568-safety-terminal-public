#pragma once

#include <QObject>
#include <QTimer>
#include <QString>

class RuntimeModel final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool valid READ valid NOTIFY changed)
    Q_PROPERTY(bool stale READ stale NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(QString cameraState READ cameraState NOTIFY changed)
    Q_PROPERTY(QString aiState READ aiState NOTIFY changed)
    Q_PROPERTY(QString alarmState READ alarmState NOTIFY changed)
    Q_PROPERTY(bool alarmActive READ alarmActive NOTIFY changed)
    Q_PROPERTY(bool roiActive READ roiActive NOTIFY changed)
    Q_PROPERTY(qulonglong framesInferred READ framesInferred NOTIFY changed)
    Q_PROPERTY(qulonglong previewSaved READ previewSaved NOTIFY changed)
    Q_PROPERTY(qulonglong previewFailed READ previewFailed NOTIFY changed)
    Q_PROPERTY(qulonglong roiEventCount READ roiEventCount NOTIFY changed)
    Q_PROPERTY(qulonglong roiClearCount READ roiClearCount NOTIFY changed)
    Q_PROPERTY(double temperatureC READ temperatureC NOTIFY changed)
    Q_PROPERTY(double humidityRh READ humidityRh NOTIFY changed)
    Q_PROPERTY(double illuminanceLux READ illuminanceLux NOTIFY changed)
    Q_PROPERTY(double previewFps READ previewFps NOTIFY changed)
    Q_PROPERTY(double previewLatencyMs READ previewLatencyMs NOTIFY changed)
    Q_PROPERTY(QString previewTransport READ previewTransport NOTIFY changed)
    Q_PROPERTY(QString previewStreamState READ previewStreamState NOTIFY changed)
    Q_PROPERTY(QString lastEventSource READ lastEventSource NOTIFY changed)
    Q_PROPERTY(QString radarState READ radarState NOTIFY changed)
    Q_PROPERTY(QString statePath READ statePath CONSTANT)

public:
    explicit RuntimeModel(QString statePath, QObject *parent = nullptr);

    bool valid() const { return valid_; }
    bool stale() const { return stale_; }
    QString error() const { return error_; }
    QString cameraState() const { return cameraState_; }
    QString aiState() const { return aiState_; }
    QString alarmState() const { return alarmState_; }
    bool alarmActive() const { return alarmActive_; }
    bool roiActive() const { return roiActive_; }
    qulonglong framesInferred() const { return framesInferred_; }
    qulonglong previewSaved() const { return previewSaved_; }
    qulonglong previewFailed() const { return previewFailed_; }
    qulonglong roiEventCount() const { return roiEventCount_; }
    qulonglong roiClearCount() const { return roiClearCount_; }
    double temperatureC() const { return temperatureC_; }
    double humidityRh() const { return humidityRh_; }
    double illuminanceLux() const { return illuminanceLux_; }
    double previewFps() const { return previewFps_; }
    double previewLatencyMs() const { return previewLatencyMs_; }
    QString previewTransport() const { return previewTransport_; }
    QString previewStreamState() const { return previewStreamState_; }
    QString lastEventSource() const { return lastEventSource_; }
    QString radarState() const { return radarState_; }
    QString statePath() const { return statePath_; }

signals:
    void changed();

private slots:
    void refresh();

private:
    QString statePath_;
    QTimer timer_;
    bool valid_ = false;
    bool stale_ = true;
    QString error_ = QStringLiteral("等待状态服务");
    QString cameraState_ = QStringLiteral("unknown");
    QString aiState_ = QStringLiteral("unknown");
    QString alarmState_ = QStringLiteral("unknown");
    bool alarmActive_ = false;
    bool roiActive_ = false;
    qulonglong framesInferred_ = 0;
    qulonglong previewSaved_ = 0;
    qulonglong previewFailed_ = 0;
    qulonglong roiEventCount_ = 0;
    qulonglong roiClearCount_ = 0;
    double temperatureC_ = 0.0;
    double humidityRh_ = 0.0;
    double illuminanceLux_ = 0.0;
    double previewFps_ = 0.0;
    double previewLatencyMs_ = 0.0;
    QString previewTransport_ = QStringLiteral("unavailable");
    QString previewStreamState_ = QStringLiteral("unavailable");
    QString lastEventSource_ = QStringLiteral("none");
    QString radarState_ = QStringLiteral("unknown");
};
