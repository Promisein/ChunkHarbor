#include "MainWindow.h"
#include "QtTcpTransport.h"
#include "chunkharbor/protocol/FrameCodec.h"
#include "chunkharbor/protocol/Handshake.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <memory>
#include <iostream>
#include <stdexcept>

void check(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}
template<class Predicate> void waitFor(Predicate predicate, int timeout = 2000,
                                      const char *message = "condition timed out") {
    QElapsedTimer clock;
    clock.start();
    while (!predicate() && clock.elapsed() < timeout) {
        QApplication::processEvents();
        QThread::msleep(1);
    }
    check(predicate(), message);
}
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    try {
        QTemporaryDir temp;
        check(temp.isValid(), "temporary workspace");
        QCoreApplication::setOrganizationName("ChunkHarborTests");
        QCoreApplication::setApplicationName("health");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temp.path());
        QSettings().setValue("storage/logDirectory", temp.filePath("logs"));
        QTcpServer server;
        check(server.listen(QHostAddress::LocalHost, 0), "listen");
        int requests = 0, closed = 0;
        bool silent = false;
        QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
            while (auto *peer = server.nextPendingConnection()) {
                auto input = std::make_shared<QByteArray>();
                QObject::connect(peer, &QTcpSocket::disconnected, &server, [&, peer] {
                    ++closed;
                    peer->deleteLater();
                });
                QObject::connect(peer, &QTcpSocket::readyRead, peer, [&, peer, input] {
                    *input += peer->readAll();
                    using namespace chunkharbor::protocol;
                    auto decoded = decode({input->constData(), static_cast<std::size_t>(input->size())});
                    if (decoded.status == DecodeStatus::NeedMore) return;
                    check(decoded.status == DecodeStatus::Complete && decoded.frame.type == 1,
                          "health must send a framed HELLO");
                    decodeHello(decoded.frame.payload);
                    ++requests;
                    if (silent) return;
                    HelloAck ack;
                    auto bytes = encode({2, decoded.frame.requestId, encodeHelloAck(ack)});
                    peer->write(bytes.data(), static_cast<qint64>(bytes.size()));
                    // A healthy HELLO endpoint may close its one-shot connection.
                    peer->disconnectFromHost();
                });
            }
        });
        MainWindow window(nullptr, temp.filePath("workspace"));
        auto *badge = window.findChild<QLabel *>("connectionBadge");
        auto *dashboard = window.findChild<QLabel *>("dashboardMetric0");
        check(badge->text() == "未检测", "startup must not claim offline without a check");
        int ready = 0, failed = 0, pulses = 0;
        QTimer pulse;
        QObject::connect(&pulse, &QTimer::timeout, &window, [&] { ++pulses; });
        pulse.start(10);
        QObject::connect(window.transport(), &QtTcpTransport::ready, &window, [&] { ++ready; });
        QObject::connect(window.transport(), &QtTcpTransport::failed, &window, [&] { ++failed; });
        window.findChild<QSpinBox *>("serverPort")->setValue(server.serverPort());
        auto *detect = window.findChild<QPushButton *>("detectConnection");
        check(detect && detect->text() == "检测连接", "explicit top bar detection action");
        detect->click();
        check(!detect->isEnabled() && detect->text() == "检测中", "pending probe disables detection");
        detect->click();
        check(!badge->property("online").toBool() && badge->text() == "检测中",
              "checking is not offline or reachable");
        waitFor([&] { return ready == 1 && closed == 1; }, 2000, "initial HELLO/close");
        QApplication::processEvents();
        check(badge->property("online").toBool(),
              "healthy HELLO followed by peer close must remain reachable, not offline");
        check(detect->isEnabled() && detect->text() == "重新检测", "completed probe can be repeated");
        check(dashboard->text() == badge->text(), "dashboard and badge agree");
        // Listening TCP alone is insufficient: the next HELLO gets no ACK.
        silent = true;
        waitFor([&] { return requests == 2; }, 18000, "periodic HELLO start");
        check(badge->text() == "检测中", "periodic check invalidates old success");
        waitFor([&] { return failed == 1 && closed == 2; }, 7000, "silent HELLO timeout");
        check(!badge->property("online").toBool() && badge->text() == "连接失败",
              "silent application must fail even when TCP accepts");
        check(detect->isEnabled() && detect->text() == "重试检测" && !badge->toolTip().isEmpty(),
              "failed probe offers retry and a reason");
        check(requests == 2 && pulses > 20, "bounded probes leave GUI event loop running");
        silent = false;
        waitFor([&] { return ready == 2 && closed == 3; }, 18000, "periodic recovery");
        check(badge->property("online").toBool(), "periodic probe recovers after failure");
        // Endpoint edits cancel an in-flight probe and its pending timeout/retry.
        silent = true;
        window.connectToLocalServer(server.serverPort());
        waitFor([&] { return requests == 4; }, 2000, "pending HELLO before endpoint edit");
        QTcpServer reserved;
        check(reserved.listen(QHostAddress::LocalHost, 0), "reserve unused port");
        auto unused = reserved.serverPort();
        reserved.close();
        window.findChild<QSpinBox *>("serverPort")->setValue(unused);
        check(badge->text() == "未检测", "endpoint edit clears previous result");
        QElapsedTimer cancelled;
        cancelled.start();
        waitFor([&] { return cancelled.elapsed() > 5300; }, 6000);
        check(failed == 1 && badge->text() == "未检测" && requests == 4,
              "cancelled timeout cannot overwrite endpoint state or retry old endpoint");
        window.connectToLocalServer(unused);
        // Windows may retry a refused loopback connect before reporting the error;
        // the application's five-second deadline must still bound the attempt.
        waitFor([&] { return failed == 2; }, 7000, "refused connection");
        check(badge->text() == "连接失败", "connection refusal is reported");
        silent = false;
        window.connectToLocalServer(server.serverPort());
        waitFor([&] { return ready == 3 && closed >= 5; }, 2000, "manual retry HELLO/close");
        check(badge->property("online").toBool(), "manual retry recovers");
        window.transport()->disconnectFromServer();
        {
            auto closing = std::make_unique<MainWindow>(nullptr, temp.filePath("closing"));
            silent = true;
            closing->connectToLocalServer(server.serverPort());
            waitFor([&] { return requests == 6; }, 2000, "destruction pending HELLO");
            closing.reset(); // Socket/timers/callbacks must die with their QObject owner.
            QApplication::processEvents();
        }
        std::cout << "HELLO peer close, periodic silent timeout/recovery, endpoint cancellation, "
                     "refusal/retry and destruction passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
