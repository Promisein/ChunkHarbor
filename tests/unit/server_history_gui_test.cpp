#include "MainWindow.h"
#include <QSpinBox>
#include "chunkharbor/protocol/Handshake.h"
#include "chunkharbor/protocol/VersionHistory.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QTableWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <iostream>
#include <memory>
#include <stdexcept>

using namespace chunkharbor::protocol;

namespace {
void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
template<class Predicate> void waitFor(Predicate predicate) {
    QElapsedTimer clock;
    clock.start();
    while (!predicate() && clock.elapsed() < 5000) {
        QApplication::processEvents();
        QThread::msleep(1);
    }
    require(predicate(), "history UI operation timed out");
}
Frame receive(QTcpSocket *socket) {
    std::string input;
    DecodeResult result;
    waitFor([&] {
        input += socket->readAll().toStdString();
        result = decode(input);
        require(result.status != DecodeStatus::Error, "invalid frame from GUI");
        return result.status == DecodeStatus::Complete;
    });
    return result.frame;
}
void send(QTcpSocket *socket, const Frame &frame) {
    const auto bytes = encode(frame);
    require(socket->write(bytes.data(), static_cast<qint64>(bytes.size())) == static_cast<qint64>(bytes.size()),
            "fake server write failed");
    socket->flush();
}
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QTemporaryDir temp;
    QCoreApplication::setOrganizationName("ChunkHarborTests");
    QCoreApplication::setApplicationName("server-history");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temp.path());
    QSettings().setValue("storage/logDirectory", temp.filePath("logs"));
    try {
        QTcpServer listener;
        require(listener.listen(QHostAddress::LocalHost), "listen");
        auto window = std::make_unique<MainWindow>(nullptr, temp.filePath("data"));
        const bool sshMode = app.arguments().size() > 1;
        window->findChild<QSpinBox *>("serverPort")->setValue(
            sshMode ? app.arguments()[1].toUShort() : listener.serverPort());
        window->findChild<QPushButton *>("versionSource")->click();
        auto *path = window->findChild<QLineEdit *>("serverHistoryPath");
        auto *first = window->findChild<QPushButton *>("serverHistoryFirst");
        auto *cancel = window->findChild<QPushButton *>("serverHistoryCancel");
        auto *rows = window->findChild<QTableWidget *>("serverHistoryRows");
        auto *status = window->findChild<QLabel *>("serverHistoryStatus");
        if (sshMode) {
            path->setText("fixture/file.txt");
            first->click();
            waitFor([&] { return rows->rowCount() == 3; });
            require(rows->item(0, 3)->text() == "7a2fb88ecfd47ffd4d45cd3aac517bf90539a7aaa5eee908028f30b6a581828f",
                    "wrong migrated current content");
            require(rows->item(0, 5)->text() == QString::fromUtf8(u8"当前") &&
                    rows->item(2, 4)->text() == QString::fromUtf8(u8"未知"), "bad current or old time");
            std::cout << "Windows history GUI via SSH: three migrated versions, current SHA and unknown old time passed\n";
            return 0;
        }
        auto query = [&](const char *name, bool supported = true) {
            path->setText(name);
            first->click();
            waitFor([&] { return listener.hasPendingConnections(); });
            auto *socket = listener.nextPendingConnection();
            const auto hello = receive(socket);
            require(hello.type == 1, "history HELLO required");
            HelloAck ack;
            if (supported) ack.capabilities.push_back(VersionHistoryCapability);
            send(socket, {2, hello.requestId, encodeHelloAck(ack)});
            if (supported) {
                const auto request = receive(socket);
                require(request.type == ListVersionsType && decodeListVersions(request.payload).path == name,
                        "history request path mismatch");
            }
            return socket;
        };
        auto *old = query("old");
        auto *fresh = query("new");
        waitFor([&] { return old->state() == QAbstractSocket::UnconnectedState; });
        send(fresh, {VersionsResultType, 2, encodeVersionsResult({"new",
            {{std::string(32, 'b'), 7, std::string(64, 'a'), 2, {}, true}}, {}})});
        waitFor([&] { return rows->rowCount() == 1; });
        require(status->text().contains(QString::fromUtf8(u8"本页 1 条")), "nonempty history page status");
        require(rows->item(0, 1)->text() == QString(32, 'b'), "stale result entered new page");
        auto *cancelled = query("cancelled");
        cancel->click();
        waitFor([&] { return cancelled->state() == QAbstractSocket::UnconnectedState; });
        require(rows->rowCount() == 0 && !cancel->isEnabled(), "cancelled result remained visible");
        query("unsupported", false);
        waitFor([&] { return !cancel->isEnabled(); });
        require(status->text().contains(QString::fromUtf8(u8"不支持")), "missing capability status");
        auto *closing = query("closing");
        window.reset();
        waitFor([&] { return closing->state() == QAbstractSocket::UnconnectedState; });
        std::cout << "history switching, cancellation, capability refusal and destruction passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
