#include "monitor_window.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDebug>
#include <QScreen>

int main(int argc, char *argv[])
{
    QApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("rk3568_hmi"));
    application.setOrganizationName(QStringLiteral("rk3568_project"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("RK3568 Wayland H.264 safety HMI"));
    parser.addHelpOption();
    QCommandLineOption runtimeOption(
        QStringList{QStringLiteral("r"), QStringLiteral("runtime-dir")},
        QStringLiteral("Unified state and media runtime directory."), QStringLiteral("directory"),
        QStringLiteral("/opt/rk3568_yolov5_demo/hmi_runtime"));
    parser.addOption(runtimeOption);
    parser.process(application);

    MonitorWindow window(parser.value(runtimeOption));
    QScreen *screen = application.primaryScreen();
    if (screen == nullptr) {
        qCritical("No Qt primary screen is available.");
        return 2;
    }

    const QRect screenGeometry = screen->geometry();
    qInfo().noquote() << QStringLiteral("Qt primary screen: %1x%2 at %3,%4")
                            .arg(screenGeometry.width())
                            .arg(screenGeometry.height())
                            .arg(screenGeometry.x())
                            .arg(screenGeometry.y());
    window.setGeometry(screenGeometry);
    window.showFullScreen();
    return application.exec();
}
