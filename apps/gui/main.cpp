#include "MainWindow.h"
#include "QtTcpTransport.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QListWidget>
#include <QLocale>
#include <QPixmap>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QTranslator>
#include <memory>

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QLocale::setDefault(QLocale(QLocale::Chinese, QLocale::China));
    QTranslator qtChinese;
    if (qtChinese.load(QStringLiteral(":/i18n/qtbase_zh_CN.qm")))
        app.installTranslator(&qtChinese);
    // Keep the prototype light even when Windows uses a dark system palette.
    QApplication::setStyle("Fusion");
    QPalette palette;
    palette.setColor(QPalette::Window, QColor("#f6f7f9"));
    palette.setColor(QPalette::WindowText, QColor("#252a34"));
    palette.setColor(QPalette::Base, Qt::white);
    palette.setColor(QPalette::Text, QColor("#252a34"));
    palette.setColor(QPalette::Button, Qt::white);
    palette.setColor(QPalette::ButtonText, QColor("#252a34"));
    palette.setColor(QPalette::Highlight, QColor("#dfeafb"));
    palette.setColor(QPalette::HighlightedText, QColor("#2167ba"));
    app.setPalette(palette);
    QCoreApplication::setApplicationName(QStringLiteral("chunkharbor-gui"));
    QCoreApplication::setOrganizationName(QStringLiteral("ChunkHarbor"));
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("ChunkHarbor 增量备份客户端"));
    parser.addHelpOption();
    parser.addOption({QStringLiteral("probe-port"),
                      QStringLiteral("连接本地服务器并在握手后退出（成功为 0，失败为 2）。"),
                      QStringLiteral("port")});
    parser.addOption({QStringLiteral("screenshot"), QStringLiteral("显示窗口后保存截图并退出。"),
                      QStringLiteral("path")});
    parser.addOption({QStringLiteral("page"),
                      QStringLiteral("初始页面：dashboard（总览）、backup（备份）、files（文件）、v"
                                     "ersions（版本历史）、logs（日志）、settings（设置）。"),
                      QStringLiteral("name"), QStringLiteral("dashboard")});
    parser.process(app);
    std::unique_ptr<QTemporaryDir> probeData;
    if (parser.isSet("probe-port")) {
        probeData = std::make_unique<QTemporaryDir>();
        if (!probeData->isValid())
            return 2;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                           probeData->path() + "/settings");
        QSettings().setValue("storage/logDirectory", probeData->path() + "/logs");
    }
    MainWindow window(nullptr, probeData ? probeData->path() : QString{});
    const QStringList pages = {"dashboard", "backup", "files", "versions", "logs", "settings"};
    const auto page = pages.indexOf(parser.value("page").toLower());
    if (page < 0)
        return 2;
    if (parser.isSet("page"))
        window.findChild<QListWidget *>("navigation")->setCurrentRow(page);
    if (parser.isSet(QStringLiteral("probe-port"))) {
        bool ok = false;
        const auto port = parser.value(QStringLiteral("probe-port")).toUInt(&ok);
        if (!ok || port == 0 || port > 65535)
            return 2;
        QObject::connect(window.transport(), &QtTcpTransport::ready, &app, [&app] { app.exit(0); });
        QObject::connect(window.transport(), &QtTcpTransport::failed, &app,
                         [&app] { app.exit(2); });
        QTimer::singleShot(0, &window, [&window, port] {
            window.connectToLocalServer(static_cast<quint16>(port));
        });
    }
    window.show();
    if (parser.isSet(QStringLiteral("screenshot"))) {
        const auto path = parser.value(QStringLiteral("screenshot"));
        QTimer::singleShot(300, &window,
                           [&window, path, &app] { app.exit(window.grab().save(path) ? 0 : 2); });
    }
    return app.exec();
}
