#include "h264_video_bridge.h"

#include <QDateTime>
#include <QGuiApplication>
#include <QWindow>
#include <QFile>
#include <QAbstractEventDispatcher>
#include <QByteArray>
#include <cstring>
#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>
#include <time.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/video/videooverlay.h>

namespace {
constexpr quint32 kMagic = 0x524b4832U;
constexpr quint32 kConfig = 1U;
constexpr quint32 kAccessUnit = 2U;
constexpr quint32 kIdr = 1U << 1;
struct Header { quint32 magic; quint32 type; quint32 flags; quint32 payloadSize; quint64 sequence; quint64 capturedAtMs; };
}

H264VideoBridge::H264VideoBridge(QString socketPath, QObject *parent)
    : QObject(parent), socketPath_(std::move(socketPath))
{
    gst_init(nullptr, nullptr);
    connect(&timer_, &QTimer::timeout, this, &H264VideoBridge::poll);
    timer_.start(10);
}

H264VideoBridge::~H264VideoBridge() { stop(); }

void H264VideoBridge::setActive(bool active)
{
    active_ = active;
    if (!active_) {
        stop();
        setState(QStringLiteral("视频未启用"));
    } else if (state_ == QStringLiteral("视频未启用")) {
        setState(QStringLiteral("等待视频"));
    }
}

void H264VideoBridge::setState(const QString &state, const QString &error)
{
    state_ = state; error_ = error; emit changed();
}

void H264VideoBridge::stop()
{
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    if (pipeline_) gst_element_set_state(pipeline_, GST_STATE_NULL);
    if (bus_) gst_object_unref(bus_); if (appsrc_) gst_object_unref(appsrc_);
    if (sink_) gst_object_unref(sink_); if (pipeline_) gst_object_unref(pipeline_);
    bus_ = nullptr; appsrc_ = nullptr; sink_ = nullptr; pipeline_ = nullptr;
}

void H264VideoBridge::reconnect()
{
    stop(); lastSequence_ = 0; configReceived_ = false; waitingIdr_ = true; frames_ = dropped_ = 0; fps_ = latencyMs_ = 0;
    setState(QStringLiteral("重连中"));
}

void H264VideoBridge::setRenderRect(int x, int y, int width, int height)
{
    rectX_ = x; rectY_ = y; rectW_ = width; rectH_ = height;
    if (sink_ && GST_IS_VIDEO_OVERLAY(sink_)) {
        gst_video_overlay_set_render_rectangle(GST_VIDEO_OVERLAY(sink_), rectX_, rectY_, rectW_, rectH_);
    }
}

void H264VideoBridge::setWindowHandle(qulonglong handle)
{
    windowHandle_ = handle;
    if (sink_ && GST_IS_VIDEO_OVERLAY(sink_)) {
        gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(sink_), static_cast<guintptr>(windowHandle_));
        setRenderRect(rectX_, rectY_, rectW_, rectH_);
    }
}

void H264VideoBridge::poll()
{
    if (!active_) return;
    if (!pipeline_) {
        GError *err = nullptr;
        pipeline_ = GST_ELEMENT(gst_parse_launch("appsrc name=src is-live=true format=time block=false ! h264parse config-interval=-1 ! mppvideodec ! waylandsink name=sink sync=false", &err));
        if (!pipeline_) { setState(QStringLiteral("解码不可用"), err ? QString::fromUtf8(err->message) : QStringLiteral("pipeline 创建失败")); if (err) g_error_free(err); return; }
        appsrc_ = gst_bin_get_by_name(GST_BIN(pipeline_), "src"); sink_ = gst_bin_get_by_name(GST_BIN(pipeline_), "sink"); bus_ = gst_element_get_bus(pipeline_);
        if (windowHandle_ && sink_ && GST_IS_VIDEO_OVERLAY(sink_)) gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(sink_), static_cast<guintptr>(windowHandle_));
        setRenderRect(rectX_, rectY_, rectW_, rectH_);
        if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) { setState(QStringLiteral("解码不可用")); stop(); return; }
    }
    if (fd_ < 0) {
        fd_ = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0);
        sockaddr_un addr{}; addr.sun_family = AF_UNIX; const QByteArray path = QFile::encodeName(socketPath_);
        if (fd_ < 0 || path.size() >= static_cast<int>(sizeof(addr.sun_path))) { setState(QStringLiteral("视频不可用"), QStringLiteral("socket 创建失败")); return; }
        std::memcpy(addr.sun_path, path.constData(), static_cast<size_t>(path.size()) + 1);
        if (::connect(fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) { ::close(fd_); fd_ = -1; setState(QStringLiteral("等待视频")); return; }
        setState(QStringLiteral("等待关键帧"));
    }
    QByteArray packet; packet.resize(2 * 1024 * 1024); const ssize_t size = ::recv(fd_, packet.data(), packet.size(), MSG_DONTWAIT);
    if (size <= 0) { if (size == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) { ::close(fd_); fd_ = -1; setState(QStringLiteral("断流")); } return; }
    if (static_cast<size_t>(size) < sizeof(Header)) { ++dropped_; return; }
    Header h{}; std::memcpy(&h, packet.constData(), sizeof(h));
    if (h.magic != kMagic || h.payloadSize != static_cast<quint32>(size - sizeof(h))) { ++dropped_; return; }
    if (h.type == kConfig) configReceived_ = true;
    else if (h.type == kAccessUnit) {
        if (!configReceived_ || (waitingIdr_ && (h.flags & kIdr) == 0)) { ++dropped_; return; }
        if (h.flags & kIdr) waitingIdr_ = false;
    } else { ++dropped_; return; }
    GstBuffer *buffer = gst_buffer_new_allocate(nullptr, h.payloadSize, nullptr);
    if (!buffer) { ++dropped_; return; }
    gst_buffer_fill(buffer, 0, packet.constData() + sizeof(h), h.payloadSize);
    if (gst_app_src_push_buffer(GST_APP_SRC(appsrc_), buffer) != GST_FLOW_OK) { ++dropped_; setState(QStringLiteral("解码错误")); return; }
    if (lastSequence_ && h.sequence > lastSequence_ + 1) dropped_ += h.sequence - lastSequence_ - 1;
    lastSequence_ = h.sequence; ++frames_; setState(QStringLiteral("播放中"));
    if (!windowStartMs_) windowStartMs_ = QDateTime::currentMSecsSinceEpoch();
    const qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - windowStartMs_; if (elapsed >= 1000) { fps_ = frames_ * 1000.0 / elapsed; windowStartMs_ = QDateTime::currentMSecsSinceEpoch(); frames_ = 0; }
    if (h.capturedAtMs) {
        timespec now{};
        if (clock_gettime(CLOCK_MONOTONIC, &now) == 0) {
            const qint64 monotonicMs = static_cast<qint64>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
            const qint64 delta = monotonicMs - static_cast<qint64>(h.capturedAtMs);
            latencyMs_ = (delta >= 0 && delta < 600000) ? static_cast<double>(delta) : 0.0;
        }
    }
    emit changed();
}
