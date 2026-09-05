#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include "runtime_model.h"
#include "h264_video_bridge.h"
#include "history_model.h"

int main(int argc, char *argv[])
{
    QGuiApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("rk3568_hmi_qml"));
    application.setOrganizationName(QStringLiteral("rk3568_project"));

    RuntimeModel runtime(QStringLiteral("/run/industrial_safety/system_state.json"));
    H264VideoBridge video(QStringLiteral("/opt/rk3568_yolov5_demo/hmi_runtime/h264.sock"));
    HistoryModel history(QStringLiteral("/run/industrial_safety/events.db"));
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("runtime"), &runtime);
    engine.rootContext()->setContextProperty(QStringLiteral("videoBridge"), &video);
    engine.rootContext()->setContextProperty(QStringLiteral("history"), &history);
    const QUrl mainQml(QStringLiteral("qrc:/qml/Main.qml"));
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreated, &application,
                     [mainQml](QObject *object, const QUrl &url) {
                         if (object == nullptr && url == mainQml) {
                             QCoreApplication::exit(1);
                         }
                     }, Qt::QueuedConnection);
    engine.load(mainQml);

    if (engine.rootObjects().isEmpty()) {
        return 1;
    }
    if (auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst())) {
        video.setWindowHandle(static_cast<qulonglong>(window->winId()));
    }
    return application.exec();
}
