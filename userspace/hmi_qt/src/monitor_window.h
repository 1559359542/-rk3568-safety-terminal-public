#pragma once

#include <QElapsedTimer>
#include <QByteArray>
#include <QJsonObject>
#include <QPixmap>
#include <QSize>
#include <QWidget>

class QLabel;
class QShowEvent;
class QSocketNotifier;
class QTimer;
struct _GstElement;
struct _GstBus;

class MonitorWindow final : public QWidget {
    Q_OBJECT

public:
    explicit MonitorWindow(QString runtimeDir, QWidget *parent = nullptr);
    ~MonitorWindow() override;

protected:
    void showEvent(QShowEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    struct RuntimeSnapshot {
        bool valid = false;
        QString error;
        qint64 monotonicMs = 0;
        QString cameraState;
        QString aiState;
        QString previewPath;
        quint64 previewSequence = 0;
        quint64 framesInferred = 0;
        quint64 previewSaved = 0;
        quint64 previewFailed = 0;
        quint64 roiEventCount = 0;
        quint64 roiClearCount = 0;
        bool roiActive = false;
        qint64 lastEventMonotonicMs = 0;
        QString lastEventSource;
        QString previewTransport;
        QString previewStreamState;
        double previewFps = 0.0;
        double previewLatencyMs = 0.0;
        bool mergedAlarmActive = false;
        QString alarmState;
        QString radarState;
        QString radarTargetState;
        QString radarCalibration;
        bool radarMetricsValid = false;
        quint64 radarDetectCm = 0;
        quint64 radarMoveCm = 0;
        quint64 radarMoveEnergy = 0;
        QString sht3xState;
        bool sht3xMetricsValid = false;
        double temperatureC = 0.0;
        double humidityRh = 0.0;
        QString bh1750State;
        bool bh1750LuxValid = false;
        double illuminanceLux = 0.0;
    };

    RuntimeSnapshot loadSnapshot() const;
    bool loadPreview(const RuntimeSnapshot &snapshot);
    void updatePreviewPixmap();
    void refresh();
    void updateStatus(const RuntimeSnapshot &snapshot, bool stateStale, bool previewStale);
    void connectH264Socket();
    void disconnectH264Socket(const QString &reason);
    void drainH264Socket();
    void pollGStreamerBus();
    bool startGStreamerPipeline();
    void stopGStreamerPipeline();
    void updateGStreamerRenderRectangle();
    void setH264Fallback(const QString &reason);
    QString resolvePreviewPath(const QString &publishedPath) const;
    static qint64 monotonicMs();
    static QString stateText(const QString &state, const QString &onlineText,
                             const QString &offlineText);

    QString runtimeDir_;
    QLabel *previewLabel_ = nullptr;
    QLabel *previewOverlay_ = nullptr;
    QLabel *summaryLabel_ = nullptr;
    QLabel *cameraLabel_ = nullptr;
    QLabel *aiLabel_ = nullptr;
    QLabel *alarmLabel_ = nullptr;
    QLabel *countsLabel_ = nullptr;
    QLabel *diagnosticLabel_ = nullptr;
    QTimer *refreshTimer_ = nullptr;
    QTimer *gstreamerBusTimer_ = nullptr;
    QSocketNotifier *h264Notifier_ = nullptr;
    int h264Fd_ = -1;
    bool h264ConfigReceived_ = false;
    bool h264WaitingForIdr_ = true;
    bool h264Active_ = false;
    bool h264StartupScheduled_ = false;
    QString h264Status_ = QStringLiteral("unavailable");
    _GstElement *gstPipeline_ = nullptr;
    _GstElement *gstAppsrc_ = nullptr;
    _GstElement *gstSink_ = nullptr;
    _GstBus *gstBus_ = nullptr;
    QPixmap preview_;
    QPixmap scaledPreview_;
    QSize scaledPreviewSize_;
    QElapsedTimer sequenceClock_;
    quint64 lastSequence_ = 0;
    bool hasSequence_ = false;
};
