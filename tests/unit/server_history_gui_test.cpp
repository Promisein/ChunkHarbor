#include "MainWindow.h"
#include <QSpinBox>
#include "chunkharbor/protocol/Handshake.h"
#include "chunkharbor/protocol/VersionHistory.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QDateTime>
#include <QClipboard>
#include <QEventLoop>
#include <QListWidget>
#include <QScrollBar>
#include <QSplitter>
#include <QTimer>
#include <QLabel>
#include <QHeaderView>
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
        const auto args = app.arguments();
        const bool sshMode = args.value(1).toUShort() != 0;
        window->findChild<QSpinBox *>("serverPort")->setValue(
            sshMode ? app.arguments()[1].toUShort() : listener.serverPort());
        auto *source = window->findChild<QPushButton *>("versionSource");
        auto *local = window->findChild<QSplitter *>("versionsSplitter");
        window->findChild<QListWidget *>("navigation")->setCurrentRow(3);
        window->show();
        source->click();
        require(local->isHidden(), "server history opens from local history");
        auto *path = window->findChild<QLineEdit *>("serverHistoryPath");
        auto *first = window->findChild<QPushButton *>("serverHistoryFirst");
        auto *next = window->findChild<QPushButton *>("serverHistoryNext");
        auto *restore = window->findChild<QPushButton *>("serverHistoryRestore");
        auto *back = window->findChild<QPushButton *>("serverHistoryBack");
        auto *empty = window->findChild<QLabel *>("serverHistoryEmpty");
        auto full = [&](const char *name) {
            return window->findChild<QLabel *>(name)->property("fullText").toString();
        };
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
        auto *paged = query("中文目录/文档.txt");
        constexpr auto committed = 1793707200ULL;
        send(paged, {VersionsResultType, 2, encodeVersionsResult({"中文目录/文档.txt",
            {{std::string(32, 'd'), 4096, std::string(64, 'e'), 9, committed, true},
             {std::string(32, 'c'), 2048, std::string(64, 'f'), 8, {}, false}}, 8})});
        waitFor([&] { return rows->rowCount() == 2; });
        require(!restore->isEnabled(), "restore requires selection");
        rows->selectRow(0);
        require(restore->isEnabled() && full("serverHistoryVersionState") == "当前" &&
                full("serverHistoryVersionTime") == QDateTime::fromSecsSinceEpoch(committed).toString("yyyy-MM-dd HH:mm:ss"),
                "current version and real server commit time");
        rows->selectRow(1);
        require(full("serverHistoryVersionId") == QString(32, 'c') &&
                full("serverHistoryVersionState") == "历史" && full("serverHistoryVersionTime") == "未知" &&
                full("serverHistoryVersionSha") == QString(64, 'f') && full("serverHistoryVersionSize") == "2.0 KiB",
                "historical detail follows the selected record");
        window->findChild<QPushButton *>("serverHistoryVersionIdCopy")->click();
        require(QApplication::clipboard()->text() == QString(32, 'c'), "copy full version ID");
        window->findChild<QPushButton *>("serverHistoryVersionShaCopy")->click();
        require(QApplication::clipboard()->text() == QString(64, 'f'), "copy full SHA-256");
        auto settle = [] {
            QEventLoop loop;
            QTimer::singleShot(100, &loop, &QEventLoop::quit);
            loop.exec();
        };
        for (const auto &size : {QSize(1280, 820), QSize(1060, 700), QSize(1440, 950)}) {
            window->resize(size);
            settle();
            auto *splitter = window->findChild<QSplitter *>("serverHistorySplitter");
            require(splitter->width() <= window->width() && rows->width() >= 360 &&
                    rows->height() <= rows->horizontalHeader()->height() + 2 * rows->rowHeight(0) + 8 &&
                    !rows->horizontalScrollBar()->isVisible(), "compact history rows fit normal and narrow windows");
        }
        window->showMaximized();
        settle();
        require(rows->isVisible() && restore->isVisible() && rows->height() < 180, "maximized short history stays compact");
        window->showNormal();
        window->resize(1280, 820);
        settle();
        if (args.value(1) == "--screenshot") require(window->grab().save(args.value(2)), "save history screenshot");
        std::cout << "server history DPR=" << window->devicePixelRatioF() << '\n';

        next->click();
        require(!restore->isEnabled() && cancel->isEnabled(), "query locks restore");
        waitFor([&] { return listener.hasPendingConnections(); });
        auto *earlier = listener.nextPendingConnection();
        const auto hello = receive(earlier);
        HelloAck ack;
        ack.capabilities.push_back(VersionHistoryCapability);
        send(earlier, {2, hello.requestId, encodeHelloAck(ack)});
        const auto request = receive(earlier);
        require(request.type == ListVersionsType && decodeListVersions(request.payload).before == 8,
                "older page uses server cursor");
        send(earlier, {VersionsResultType, 2, encodeVersionsResult({"中文目录/文档.txt",
            {{std::string(32, '1'), 256, std::string(64, '2'), 7, {}, false}}, {}})});
        waitFor([&] { return rows->rowCount() == 1 && !cancel->isEnabled(); });
        require(!restore->isEnabled() && !next->isEnabled() && full("serverHistoryVersionSha").isEmpty(),
                "page replacement clears stale selection and cursor");
        back->click();
        require(local->isVisible() && source->isVisible() && !rows->isVisible(),
                "return restores the local page and hides server results");
        source->click();
        auto *many = query("many");
        VersionsResult longPage{"many", {}, {}};
        for (int i = 32; i > 0; --i) {
            longPage.versions.push_back({QString::number(i, 16).rightJustified(32, '0').toStdString(),
                4096, std::string(64, 'a'), static_cast<std::uint64_t>(i), {}, i == 32});
        }
        send(many, {VersionsResultType, 2, encodeVersionsResult(longPage)});
        waitFor([&] { return rows->rowCount() == 32 && rows->verticalScrollBar()->maximum() > 0; });
        rows->scrollToBottom();
        require(rows->verticalScrollBar()->value() == rows->verticalScrollBar()->maximum(),
                "full page scrolls to its final version");
        auto *missing = query("missing");
        send(missing, {VersionsResultType, 2, encodeVersionsResult({"missing", {}, {}})});
        waitFor([&] { return !cancel->isEnabled(); });
        require(rows->rowCount() == 0 && empty->isVisible() && status->text().contains("没有"), "empty page hint");
        auto *disconnected = query("disconnected");
        disconnected->abort();
        waitFor([&] { return !cancel->isEnabled(); });
        require(empty->isVisible() && !status->text().isEmpty() && !restore->isEnabled(), "disconnect is reported");
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
