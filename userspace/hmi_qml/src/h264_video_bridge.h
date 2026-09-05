#pragma once

#include <QObject>
#include <QString>
#include <QTimer>

struct _GstElement;
struct _GstBus;

class H264VideoBridge final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString state READ state NOTIFY changed)
    Q_PROPERTY(double fps READ fps NOTIFY changed)
    Q_PROPERTY(double latencyMs READ latencyMs NOTIFY changed)
    Q_PROPERTY(qulonglong frames READ frames NOTIFY changed)
    Q_PROPERTY(qulonglong dropped READ dropped NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
public:
    explicit H264VideoBridge(QString socketPath, QObject *parent = nullptr);
    ~H264VideoBridge() override;
    QString state() const { return state_; }
    double fps() const { return fps_; }
    double latencyMs() const { return latencyMs_; }
    qulonglong frames() const { return frames_; }
    qulonglong dropped() const { return dropped_; }
    QString error() const { return error_; }
    Q_INVOKABLE void reconnect();
    Q_INVOKABLE void setActive(bool active);
    Q_INVOKABLE void setWindowHandle(qulonglong handle);
    Q_INVOKABLE void setRenderRect(int x, int y, int width, int height);
signals:
    void changed();
private slots:
    void poll();
private:
    void stop();
    void setState(const QString &state, const QString &error = {});
    QString socketPath_;
    QTimer timer_;
    int fd_ = -1;
    _GstElement *pipeline_ = nullptr;
    _GstElement *appsrc_ = nullptr;
    _GstElement *sink_ = nullptr;
    _GstBus *bus_ = nullptr;
    QString state_ = QStringLiteral("等待视频");
    QString error_;
    double fps_ = 0.0;
    double latencyMs_ = 0.0;
    qulonglong frames_ = 0;
    qulonglong dropped_ = 0;
    quint64 lastSequence_ = 0;
    bool configReceived_ = false;
    bool waitingIdr_ = true;
    qint64 windowStartMs_ = 0;
    int rectX_ = 0;
    int rectY_ = 0;
    int rectW_ = 0;
    int rectH_ = 0;
    qulonglong windowHandle_ = 0;
    bool active_ = false;
};
