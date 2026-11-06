// A private child process emulating SSH lifetime, never reads user SSH configuration.
#include "chunkharbor/protocol/Handshake.h"
#include <QCoreApplication>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <iostream>
#include <memory>
#ifdef Q_OS_WIN
#include <windows.h>
#endif
int main(int argc, char **argv) {
#ifdef Q_OS_WIN
    if (GetConsoleWindow()) return 17; // QProcess must create this console binary without a console.
#endif
    QCoreApplication app(argc, argv);
    const auto arguments = app.arguments();
    const auto mode = arguments.last();
    if (mode == "auth" || mode == "hostkey") {
        std::cerr << (mode == "auth" ? "Permission denied (publickey).\n" : "Host key verification failed.\n");
        return 255;
    }
    if (mode == "never") return app.exec();
    if (mode == "lateauth") {
        QTimer::singleShot(150, &app, [&] { std::cerr << "Permission denied (publickey).\n" << std::flush; app.exit(255); });
        return app.exec();
    }
    if (mode == "flood") { std::cerr << std::string(70000, 'x') << std::flush; return app.exec(); }
    const auto index = arguments.indexOf("-L");
    if (index < 0 || index+1 >= arguments.size()) return 64;
    const auto port = arguments[index+1].section(':', 1, 1).toUShort();
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost, port)) { std::cerr << "Address already in use\n"; return 255; }
    QObject::connect(&server, &QTcpServer::newConnection, &app, [&] {
        while (auto *peer = server.nextPendingConnection()) {
            auto input = std::make_shared<QByteArray>();
            QObject::connect(peer, &QTcpSocket::disconnected, peer, &QObject::deleteLater);
            QObject::connect(peer, &QTcpSocket::readyRead, peer, [=] {
                *input += peer->readAll();
                using namespace chunkharbor::protocol;
                const auto frame = decode({input->constData(), static_cast<size_t>(input->size())});
                if (frame.status != DecodeStatus::Complete || mode == "silent") return;
                const auto bytes = encode({2, frame.frame.requestId, encodeHelloAck(HelloAck{})});
                peer->write(bytes.data(), static_cast<qint64>(bytes.size()));
                peer->disconnectFromHost();
            });
        }
    });
    if (mode == "crash") QTimer::singleShot(300, &app, [&] { app.exit(42); });
    return app.exec();
}
