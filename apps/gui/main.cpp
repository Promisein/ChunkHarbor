#include "MainWindow.h"
#include "QtTcpTransport.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QPixmap>
#include <QTimer>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("chunkharbor-gui"));
    QCommandLineParser parser; parser.setApplicationDescription(QStringLiteral("ChunkHarbor M2 connection prototype")); parser.addHelpOption();
    parser.addOption({QStringLiteral("probe-port"), QStringLiteral("Connect to localhost and exit after handshake (0 success, 2 failure)."), QStringLiteral("port")});
    parser.addOption({QStringLiteral("screenshot"), QStringLiteral("Save a window screenshot after showing it."), QStringLiteral("path")});
    parser.process(app);
    MainWindow window;
    if (parser.isSet(QStringLiteral("probe-port"))) {
        bool ok = false; const auto port = parser.value(QStringLiteral("probe-port")).toUInt(&ok);
        if (!ok || port == 0 || port > 65535) return 2;
        QObject::connect(window.transport(), &QtTcpTransport::ready, &app, [&app] { app.exit(0); });
        QObject::connect(window.transport(), &QtTcpTransport::failed, &app, [&app] { app.exit(2); });
        QTimer::singleShot(0, &window, [&window, port] { window.connectToLocalServer(static_cast<quint16>(port)); });
    }
    window.show();
    if (parser.isSet(QStringLiteral("screenshot"))) {
        const auto path = parser.value(QStringLiteral("screenshot"));
        QTimer::singleShot(300, &window, [&window, path, &app] { app.exit(window.grab().save(path) ? 0 : 2); });
    }
    return app.exec();
}
