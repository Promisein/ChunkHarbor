#include "MainWindow.h"
#include "SnapshotPreflight.h"
#include "chunkharbor/protocol/DirectorySnapshot.h"
#include "chunkharbor/protocol/Handshake.h"
#include "chunkharbor/protocol/Download.h"
#include <QAbstractButton>
#include <QApplication>
#include <QClipboard>
#include <QEventLoop>
#include <QScrollBar>
#include <QSplitter>
#include <QTimer>
#include <QCryptographicHash>
#include <QDir>
#include <QDialog>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTableView>
#include <QTableWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <algorithm>
#include <iostream>
#include <memory>
#include <stdexcept>
#ifdef Q_OS_WIN
#include <windows.h>
#include <psapi.h>
#endif

using namespace chunkharbor::protocol;
namespace {
void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
template<class Predicate> void waitFor(Predicate predicate) {
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < 5000) {
        if (predicate()) return;
        QApplication::processEvents();
        if (predicate()) return;
        QThread::msleep(1);
    }
    require(predicate(), "snapshot UI operation timed out");
}
Frame receive(QTcpSocket *socket) {
    std::string input;
    DecodeResult result;
    waitFor([&] {
        input += socket->readAll().toStdString();
        result = decode(input);
        require(result.status != DecodeStatus::Error, "invalid frame from snapshot GUI");
        return result.status == DecodeStatus::Complete;
    });
    return result.frame;
}
void send(QTcpSocket *socket, std::uint16_t type, std::uint64_t rid, std::string body) {
    const auto bytes = encode({type, rid, std::move(body)});
    require(socket->write(bytes.data(), static_cast<qint64>(bytes.size())) == static_cast<qint64>(bytes.size()),
            "fake server write failed");
    socket->flush();
}
ListedFile file(std::string path) {
    return {std::move(path), std::string(32, 'a'), 1, std::string(64, 'b')};
}
void writeFile(const QString &name) {
    QFile f(name);
    require(f.open(QIODevice::WriteOnly) && f.write("preserve") == 8, "local fixture write failed");
}
QByteArray readFile(const QString &name) {
    QFile file(name);
    require(file.open(QIODevice::ReadOnly), "cannot read restored file");
    return file.readAll();
}
QByteArray sha256(QByteArrayView bytes) { return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256); }
}
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QTemporaryDir temp;
    QCoreApplication::setOrganizationName("ChunkHarborTests");
    QCoreApplication::setApplicationName("snapshot-preview");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temp.path());
    const auto directHost = qEnvironmentVariable("CHUNKHARBOR_TEST_TCP_HOST");
    QSettings().setValue("connection/mode", "tcp");
    QSettings().setValue("connection/tcpHost", directHost.isEmpty() ? "127.0.0.1" : directHost);
    if (!directHost.isEmpty()) {
        QSettings().setValue("connection/mode", "tcp");
        QSettings().setValue("connection/tcpHost", directHost);
    }
    QSettings().setValue("storage/logDirectory", temp.filePath("logs"));
    try {
        QTcpServer listener;
        require(listener.listen(QHostAddress::LocalHost), "listen failed");
        auto window = std::make_unique<MainWindow>(nullptr, temp.filePath("workspace"), MainWindow::StartupMode::Manual);
        const bool remote = app.arguments().value(1).toUShort() != 0;
        auto *serverPort = window->findChild<QSpinBox *>("serverPort");
        serverPort->setValue(
            remote ? app.arguments()[1].toUShort() : listener.serverPort());
        window->findChild<QListWidget *>("navigation")->setCurrentRow(4);
        auto *root = window->findChild<QLineEdit *>("snapshotRoot");
        auto *page = root->parentWidget();
        auto *target = window->findChild<QLineEdit *>("snapshotTarget");
        auto *query = window->findChild<QPushButton *>("snapshotQuery");
        auto *cancel = window->findChild<QPushButton *>("snapshotCancel");
        auto *capture = window->findChild<QPushButton *>("snapshotDownloadFolder");
        require(capture, "snapshot page must expose current folder download");
        auto *restore = window->findChild<QPushButton *>("snapshotRestore");
#ifdef Q_OS_WIN
        require(restore && !restore->isEnabled(), "restore must be unavailable before a verified manifest");
#endif
        auto *status = window->findChild<QLabel *>("snapshotStatus");
        auto *list = window->findChild<QTableWidget *>("snapshotList");
        auto *model = window->findChild<QTableView *>("snapshotEntries")->model();
        auto complete = [&] { return page->property("manifestComplete").toBool() && !cancel->isEnabled(); };
        auto cell = [&](int row, int column) { return model->index(row, column).data().toString(); };
#ifdef Q_OS_WIN
        auto *name = window->findChild<QLineEdit *>("snapshotNewDirectory");
        auto *restoreStatus = window->findChild<QLabel *>("snapshotRestoreStatus");
        auto *restoreCancel = window->findChild<QPushButton *>("snapshotRestoreCancel");
        auto startCapture = [&](bool accept = true) {
            require(capture->isEnabled(), "current folder capture unavailable");
            capture->click();
            auto *box = window->findChild<QMessageBox *>();
            require(box && box->text().contains(QString::fromUtf8(u8"持久快照")) &&
                    !capture->isEnabled(), "capture requires persistent snapshot consent");
            capture->click();
            box->button(accept ? QMessageBox::Yes : QMessageBox::Cancel)->click();
            QApplication::processEvents();
        };
        auto startRestore = [&](const QString &directory) {
            name->setText(directory);
            waitFor([&] { return restore->isEnabled(); });
            restore->click();
            restore->click(); // only one confirmation and one worker
            auto *box = window->findChild<QMessageBox *>();
            require(box && !restore->isEnabled(), "restore must be gated by confirmation");
            box->button(QMessageBox::Yes)->click();
            waitFor([&] { return restoreCancel->isEnabled() || !restoreStatus->text().isEmpty(); });
        };
        auto finished = [&](const QString &outcome) {
            waitFor([&] { return !restoreCancel->isEnabled(); });
            if (!restoreStatus->text().contains(outcome))
                throw std::runtime_error(restoreStatus->text().toStdString());
        };
        auto noStaging = [&] {
            require(QDir(temp.path()).entryList({".chunkharbor-restore-*"},
                QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot).empty(), "staging directory leaked");
        };
#endif
        root->setText("fixture");
        target->setText(temp.path());
        if (remote) {
#ifdef Q_OS_WIN
            if (app.arguments().contains("--download-folder")) {
                startCapture();
                waitFor(complete);
                require(list->rowCount() == 1 && model->rowCount() == 3 &&
                        cell(0, 0) == "file.txt" &&
                        cell(0, 3) == "7a2fb88ecfd47ffd4d45cd3aac517bf90539a7aaa5eee908028f30b6a581828f" &&
                        cell(2, 0) == QString::fromUtf8(u8"子目录/文件.txt"),
                        "current folder manifest incorrect or crossed prefix");
                const auto captured = list->item(0, 0)->text();
                startRestore("current");
                finished(QString::fromUtf8(u8"恢复成功"));
                require(restoreStatus->text().contains(captured), "download lost capture ID");
                const std::vector<std::pair<QString, QByteArray>> expected{
                    {"file.txt", "R2.2 fixture C"}, {"legacy.txt", "legacy fixture"},
                    {QString::fromUtf8(u8"子目录/文件.txt"), "nested"}};
                for (const auto &[path, content] : expected) {
                    const auto actual = readFile(temp.filePath("current/" + path));
                    require(actual == content, "current folder bytes changed after capture");
                    std::cout << path.toUtf8().constData() << " SHA-256 " << sha256(actual).toHex().constData() << '\n';
                }
                require(!QFile::exists(temp.filePath("current/excluded.txt")), "sibling prefix included");
                noStaging();
                std::cout << "Windows SSH current folder snapshot " << captured.toStdString() << " restored\n";
                return 0;
            }
#endif
            query->click();
            waitFor([&] { return list->rowCount() >= 1; });
            const bool execute = app.arguments().contains("--restore-snapshot");
#ifdef Q_OS_WIN
            if (execute) {
                require(list->rowCount() == 2, "SSH restore expected two snapshots");
                list->setCurrentCell(0, 0);
                waitFor(complete);
                startRestore("newer");
                finished(QString::fromUtf8(u8"恢复成功"));
                require(readFile(temp.filePath("newer/file.txt")) == "DDDD", "new snapshot did not restore D");
                std::cout << "new snapshot file.txt SHA-256 "
                          << sha256(readFile(temp.filePath("newer/file.txt"))).toHex().constData() << '\n';
            }
#endif
            const auto row = execute ? 1 : 0;
            const auto id = list->item(row, 0)->text();
            list->setCurrentCell(row, 0);
            waitFor(complete);
            require(model->rowCount() == 2 && cell(0, 0) == "file.txt" && cell(1, 0) == "legacy.txt" &&
                    cell(0, 1) == "72efe174559bd35db95e227abdf7ae99" &&
                    cell(0, 3) == "7a2fb88ecfd47ffd4d45cd3aac517bf90539a7aaa5eee908028f30b6a581828f" &&
                    status->text().contains(id), "SSH preview lost selected snapshot mapping");
            require(!QFile::exists(temp.filePath("file.txt")), "preview wrote a file");
#ifdef Q_OS_WIN
            if (execute) {
                startRestore("original");
                finished(QString::fromUtf8(u8"恢复成功"));
                const auto original = readFile(temp.filePath("original/file.txt"));
                const auto legacy = readFile(temp.filePath("original/legacy.txt"));
                require(original == "R2.2 fixture C" && legacy == "legacy fixture" &&
                    sha256(original).toHex() == "7a2fb88ecfd47ffd4d45cd3aac517bf90539a7aaa5eee908028f30b6a581828f",
                    "old snapshot bytes or SHA-256 mismatch");
                noStaging();
                std::cout << "old snapshot file.txt SHA-256 " << sha256(original).toHex().constData()
                          << ", legacy.txt SHA-256 " << sha256(legacy).toHex().constData() << '\n';
            }
#endif
            std::cout << "Windows SSH snapshot preview: " << id.toStdString()
                      << ", 2 files, 28 bytes, original C SHA-256 preserved after newer upload\n";
            return 0;
        }

        const auto hello = [&](bool supported = true) {
            waitFor([&] { return listener.hasPendingConnections(); });
            auto *socket = listener.nextPendingConnection();
            const auto request = receive(socket);
            const auto offer = decodeHello(request.payload);
            require(request.type == 1 && std::find(offer.capabilities.begin(), offer.capabilities.end(),
                    DirectorySnapshotCapability) != offer.capabilities.end(), "snapshot HELLO capability missing");
            HelloAck ack;
            if (supported) ack.capabilities.push_back(DirectorySnapshotCapability);
            send(socket, 2, request.requestId, encodeHelloAck(ack));
            return socket;
        };
        query->click();
        auto *unsupported = hello(false);
        waitFor([&] { return unsupported->state() == QAbstractSocket::UnconnectedState; });
        require(status->text().contains(QString::fromUtf8(u8"不支持")), "old server must report unavailable");

        std::vector<ListedFile> complex{file("Case.txt"), file("case.TXT"), file("CON.txt"), file("bad?.txt"),
            file("parent"), file("parent/child"), file("exists.txt"), file("blocked/child"),
            file("Alias/a"), file("alias/b"), file(u8"中文/文件.txt"), file(std::string(230, 'p'))};
        writeFile(temp.filePath("exists.txt"));
        writeFile(temp.filePath("blocked"));
#ifdef Q_OS_WIN
        const auto junction = temp.filePath("junction");
        require(QDir().mkpath(temp.filePath("junction-target")), "junction target fixture");
        require(QProcess::execute("cmd.exe", {"/d", "/c", "mklink", "/J",
            QDir::toNativeSeparators(junction), QDir::toNativeSeparators(temp.filePath("junction-target"))}) == 0,
            "junction fixture");
        complex.push_back(file("junction/child"));
#endif
        std::sort(complex.begin(), complex.end(), [](const auto &a, const auto &b) { return a.path < b.path; });
        const SnapshotInfo large{std::string(32, 'd'), "fixture", 4, 10, 10000};
        const SnapshotInfo mixed{std::string(32, 'c'), "fixture", 3, 10, static_cast<std::uint32_t>(complex.size())};
        const SnapshotInfo single{std::string(32, 'b'), "fixture", 2, 10, 1};
        const SnapshotInfo empty{std::string(32, 'a'), "fixture", 1, 10, 0};
        const SnapshotInfo nested{std::string(32, 'e'), "fixture", 5, 10, 129};
        query->click();
        auto *listPeer = hello();
        const auto request = receive(listPeer);
        require(request.type == ListSnapshotsType && decodeListSnapshots(request.payload).root == "fixture",
                "wrong snapshot list request");
        send(listPeer, SnapshotsPageType, request.requestId,
             encodeSnapshotsPage({"fixture", {nested, large, mixed, single, empty}, {}}));
        waitFor([&] { return list->rowCount() == 5; });

        const auto open = [&](int row, const SnapshotInfo &info) {
            list->setCurrentCell(row, 0);
            auto *socket = hello();
            const auto get = receive(socket);
            require(get.type == GetSnapshotType && decodeGetSnapshot(get.payload) == info.id,
                    "wrong metadata request");
            send(socket, SnapshotInfoType, get.requestId, encodeSnapshotInfo(info));
            return socket;
        };
        const auto pageReply = [&](QTcpSocket *socket, const SnapshotInfo &info, const std::string &after,
                                   std::vector<ListedFile> items, std::string next = {}) {
            const auto pull = receive(socket);
            const auto filter = decodeListSnapshotEntries(pull.payload);
            require(pull.type == ListSnapshotEntriesType && filter.snapshotId == info.id &&
                    filter.afterPath == after && filter.limit == MaxSnapshotEntriesPerPage, "wrong manifest cursor");
            send(socket, SnapshotEntriesPageType, pull.requestId,
                 encodeSnapshotEntriesPage({info.id, std::move(items), std::move(next)}));
        };

        if (app.arguments().value(1) == "--layout") {
            auto *entries = window->findChild<QTableView *>("snapshotEntries");
            auto *details = window->findChild<QWidget *>("snapshotDetails");
            auto *recovery = window->findChild<QWidget *>("snapshotRecoveryPanel");
            auto *older = window->findChild<QPushButton *>("snapshotOlder");
            auto *nextFiles = window->findChild<QPushButton *>("snapshotEntriesNext");
            auto *previousFiles = window->findChild<QPushButton *>("snapshotEntriesPrevious");
            auto full = [&](const char *label) {
                return window->findChild<QLabel *>(label)->property("fullText").toString();
            };
            auto settle = [] {
                QEventLoop loop;
                QTimer::singleShot(80, &loop, &QEventLoop::quit);
                loop.exec();
            };
            auto fits = [&](QWidget *widget) {
                const auto rect = QRect(widget->mapTo(window.get(), QPoint{}), widget->size());
                const auto *bar = window->findChild<QWidget *>("filesTransferBar");
                return widget->isVisible() && window->rect().contains(rect) &&
                    rect.bottom() < bar->mapTo(window.get(), QPoint{}).y();
            };
            auto checkGeometry = [&] {
                require(fits(details) && fits(list) && fits(entries) && fits(recovery) && fits(restore) &&
                        !entries->horizontalScrollBar()->isVisible() && !list->horizontalScrollBar()->isVisible() &&
                        list->viewport()->height() >= list->rowHeight(0) &&
                        entries->viewport()->height() >= entries->rowHeight(0),
                        "both lists and fixed areas fit without horizontal overflow");
            };
            auto showManifest = [&](int row, const SnapshotInfo &info) {
                auto *socket = open(row, info);
                std::string after;
                for (std::uint32_t offset = 0; offset < info.fileCount; offset += MaxSnapshotEntriesPerPage) {
                    const auto end = std::min(info.fileCount, offset + MaxSnapshotEntriesPerPage);
                    std::vector<ListedFile> batch;
                    for (auto i = offset; i < end; ++i) {
                        const auto suffix = i == 0 ? std::string(130, 'p') : std::string("report");
                        batch.push_back(file(std::to_string(10000 + i) + "-" + suffix + ".txt"));
                    }
                    const auto next = end < info.fileCount ? batch.back().path : "";
                    pageReply(socket, info, after, std::move(batch), next);
                    after = next;
                }
                waitFor(complete);
                require(full("snapshotSelectedId") == QString::fromStdString(info.id) &&
                        model->rowCount() == static_cast<int>(std::min(info.fileCount, 100U)) &&
                        cell(0, 0).startsWith("10000-") && !restore->isEnabled(),
                        "selected manifest follows the server and stays in read-only preview");
            };
            auto privateMiB = []() -> double {
#ifdef Q_OS_WIN
                PROCESS_MEMORY_COUNTERS_EX counters{};
                if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters),
                                         sizeof(counters)))
                    return counters.PrivateUsage / (1024.0 * 1024.0);
#endif
                return -1;
            };
            target->clear();
            window->show();
            const std::pair<int, unsigned> cases[]{{1, 2}, {20, 100}, {100, 1000}, {500, 5000}};
            QSize fixedHeights;
            for (const auto &[historyCount, fileCount] : cases) {
                window->showNormal();
                window->resize(1280, 820);
                settle();
                std::vector<SnapshotInfo> history;
                for (int sequence = historyCount; sequence > 0; --sequence) {
                    history.push_back({QString::number(sequence, 16).rightJustified(32, '0').toStdString(),
                        "fixture", static_cast<std::uint64_t>(sequence), 1793707200ULL,
                        sequence == historyCount ? fileCount : 2U});
                }
                auto listPage = [&](int offset, bool firstPage) {
                    auto *socket = hello();
                    const auto frame = receive(socket);
                    const auto request = decodeListSnapshots(frame.payload);
                    const auto cursor = firstPage ? std::optional<std::uint64_t>{} :
                        std::optional<std::uint64_t>{history[offset - 1].sequence};
                    require(frame.type == ListSnapshotsType && request.before == cursor && request.limit == 32,
                            "history stress fixture uses real cursors and the production page limit");
                    const auto end = std::min(offset + static_cast<int>(request.limit), historyCount);
                    send(socket, SnapshotsPageType, frame.requestId,
                         encodeSnapshotsPage({"fixture", {history.begin() + offset, history.begin() + end},
                             end < historyCount ? std::optional<std::uint64_t>{history[end - 1].sequence} : std::nullopt}));
                    waitFor([&] { return !cancel->isEnabled() && list->rowCount() == end - offset; });
                    require(list->item(0, 0)->text() == QString::fromStdString(history[offset].id) &&
                            list->item(list->rowCount() - 1, 0)->text() == QString::fromStdString(history[end - 1].id),
                            "history page displays its first and last record");
                    return end;
                };
                QElapsedTimer elapsed, pulseClock;
                elapsed.start();
                pulseClock.start();
                qint64 worstGap = 0;
                QTimer pulse;
                QObject::connect(&pulse, &QTimer::timeout, &pulse,
                                 [&] { worstGap = qMax(worstGap, pulseClock.restart()); });
                pulse.start(10);
                query->click();
                int seen = listPage(0, true);
                if (historyCount > 1) showManifest(1, history[1]);
                showManifest(0, history[0]);
                settle();
                checkGeometry();
                if (!fixedHeights.isValid()) fixedHeights = {details->height(), recovery->height()};
                const double loadedMiB = privateMiB();
                require(QSize(details->height(), recovery->height()) == fixedHeights,
                        "data volume leaves the fixed section heights stable");
                const int listHeight = list->height(), entriesHeight = entries->height();
                const int normalWindowHeight = window->height();
                window->resize(1280, 920);
                settle();
                checkGeometry();
                const int heightGrowth = window->height() - normalWindowHeight;
                require(heightGrowth >= 40 && list->height() >= listHeight + heightGrowth - 2 &&
                        entries->height() >= entriesHeight + heightGrowth - 2 &&
                        QSize(details->height(), recovery->height()) == fixedHeights,
                        "additional height goes to both lists");
                window->resize(1060, 700);
                settle();
                checkGeometry();
                window->showMaximized();
                settle();
                checkGeometry();
                window->showNormal();
                window->resize(1280, 820);
                settle();
                if (historyCount > 1) {
                    auto *leftScroll = list->verticalScrollBar();
                    auto *rightScroll = entries->verticalScrollBar();
                    require(leftScroll->maximum() > 0 && rightScroll->maximum() > 0, "both populated tables scroll");
                    rightScroll->setValue(0);
                    leftScroll->setValue(leftScroll->maximum());
                    require(rightScroll->value() == 0, "history scroll is independent");
                    leftScroll->setValue(0);
                    rightScroll->setValue(rightScroll->maximum());
                    require(leftScroll->value() == 0, "manifest scroll is independent");
                    rightScroll->setValue(0);
                }
                if (fileCount > 100) {
                    nextFiles->click();
                    require(model->rowCount() == 100 && cell(0, 0).startsWith("10100-"), "manifest pagination");
                    previousFiles->click();
                }
                if (historyCount == 500 && app.arguments().size() > 2)
                    require(window->grab().save(app.arguments()[2]), "snapshot layout screenshot");
                while (seen < historyCount) {
                    require(older->isEnabled(), "more history remains accessible");
                    older->click();
                    seen = listPage(seen, false);
                }
                require(!older->isEnabled(), "last history page has no next cursor");
                pulse.stop();
                std::cerr << "snapshot layout DPR=" << window->devicePixelRatioF()
                          << " history=" << seen << " files=" << fileCount << " elapsedMs=" << elapsed.elapsed()
                          << " maxPulseGapMs=" << worstGap << " loadedPrivateMiB=" << loadedMiB
                          << " afterPagingPrivateMiB=" << privateMiB() << '\n';
            }
            return 0;
        }

        auto *stale = open(1, large);
        const auto stalePull = receive(stale);
        require(stalePull.type == ListSnapshotEntriesType, "missing first pull");
        auto *complexPeer = open(2, mixed);
        waitFor([&] { return stale->state() == QAbstractSocket::UnconnectedState; });
        std::string cursor;
        for (std::size_t offset = 0; offset < complex.size(); offset += MaxSnapshotEntriesPerPage) {
            const auto end = std::min(complex.size(), offset + MaxSnapshotEntriesPerPage);
            const auto next = end < complex.size() ? complex[end - 1].path : "";
            pageReply(complexPeer, mixed, cursor, {complex.begin() + offset, complex.begin() + end}, next);
            cursor = next;
        }
        waitFor(complete);
        require(model->rowCount() == static_cast<int>(complex.size()), "complex preview count");
        auto lookup = [&](const QString &path, int col) {
            for (int row = 0; row < model->rowCount(); ++row)
                if (cell(row, 0) == path) return cell(row, col);
            return QString{};
        };
        require(lookup("CON.txt", 5).contains(QString::fromUtf8(u8"非法")) &&
                lookup("bad?.txt", 5).contains(QString::fromUtf8(u8"非法")) &&
                lookup(QString(230, 'p'), 5).contains(QString::fromUtf8(u8"过长")), "invalid Windows paths");
        require(lookup("CON.txt", 6).contains(QString::fromUtf8(u8"未检查")),
                "invalid path must not appear conflict-free");
        require(lookup("Case.txt", 6).contains(QString::fromUtf8(u8"碰撞")) &&
                lookup("parent", 6).contains(QString::fromUtf8(u8"冲突")) &&
                lookup("Alias/a", 6).contains(QString::fromUtf8(u8"冲突")), "case or file-directory collision");
#ifdef Q_OS_WIN
        require(lookup("exists.txt", 6).contains(QString::fromUtf8(u8"已存在")) &&
                lookup("blocked/child", 6).contains(QString::fromUtf8(u8"占用")) &&
                lookup("junction/child", 6).contains(QString::fromUtf8(u8"重解析点")), "local conflict checks");
#endif
        require(lookup(QString::fromUtf8(u8"中文/文件.txt"), 4).endsWith(QString::fromUtf8(u8"中文/文件.txt")),
                "UTF-8 target mapping");
        QFile existing(temp.filePath("exists.txt"));
        require(existing.open(QIODevice::ReadOnly) && existing.readAll() == "preserve", "preview altered existing file");
        require(!QFile::exists(temp.filePath(QString::fromUtf8(u8"中文"))), "preview created a directory");

        auto *emptyPeer = open(4, empty);
        pageReply(emptyPeer, empty, "", {});
        waitFor(complete);
        require(model->rowCount() == 0 && status->text().contains(QString::fromUtf8(u8"空快照")), "empty snapshot");
#ifdef Q_OS_WIN
        startRestore("empty");
        finished(QString::fromUtf8(u8"恢复成功"));
        require(QDir(temp.filePath("empty")).exists() &&
            QDir(temp.filePath("empty")).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty(),
            "empty snapshot did not publish an empty directory");
        name->clear();
        noStaging();
        auto readyFile = [](std::string path, const QByteArray &bytes) {
            return ListedFile{std::move(path), std::string(32, 'a'), static_cast<quint64>(bytes.size()),
                sha256(bytes).toHex().toStdString()};
        };
        auto acceptDownload = [&](const ListedFile &entry) {
            waitFor([&] { return listener.hasPendingConnections() || !restoreCancel->isEnabled(); });
            if (!listener.hasPendingConnections()) throw std::runtime_error(restoreStatus->text().toStdString());
            auto *socket = listener.nextPendingConnection();
            const auto handshake = receive(socket);
            const auto offer = decodeHello(handshake.payload);
            require(handshake.type == 1 && offer.capabilities.back() == HistoricalDownloadCapability,
                    "restore must use historical download negotiation");
            HelloAck ack;
            ack.capabilities = {HistoricalDownloadCapability};
            send(socket, 2, handshake.requestId, encodeHelloAck(ack));
            const auto begin = receive(socket);
            const auto selected = decodeBeginHistoricalDownload(begin.payload);
            require(begin.type == BeginHistoricalDownloadType && selected.path == "fixture/" + entry.path &&
                selected.versionId == entry.versionId, "download escaped snapshot version mapping");
            send(socket, 32, begin.requestId, encodeDownloadReady({selected.path, entry.versionId, entry.size, entry.sha256}));
            return socket;
        };
        auto deliver = [&](QTcpSocket *socket, const ListedFile &entry, const QByteArray &content, bool valid = true) {
            const auto get = receive(socket);
            const auto requested = decodeGetDownloadChunk(get.payload);
            require(get.type == 33 && requested.versionId == entry.versionId &&
                requested.offset == 0 && requested.length == entry.size, "wrong restored chunk request");
            DownloadChunk chunk{entry.versionId, 0, {}, content.toStdString()};
            const auto digest = sha256(content);
            if (valid) std::copy(digest.begin(), digest.end(), chunk.sha256.begin());
            send(socket, 14, get.requestId, encodeDownloadChunk(chunk));
        };
        const auto first = readyFile("only", "first");
#endif
        auto *one = open(3, single);
#ifdef Q_OS_WIN
        pageReply(one, single, "", {first});
#else
        pageReply(one, single, "", {file("only")});
#endif
        waitFor(complete);
        require(model->rowCount() == 1 && cell(0, 0) == "only", "single file snapshot");
        auto *entries = window->findChild<QTableView *>("snapshotEntries");
        auto labelValue = [&](const char *label) {
            return window->findChild<QLabel *>(label)->property("fullText").toString();
        };
        require(labelValue("snapshotSelectedId") == QString::fromStdString(single.id) &&
                labelValue("snapshotSelectedRoot") == "fixture", "summary follows selected snapshot");
        entries->selectRow(0);
        require(labelValue("snapshotEntryVersionId") == cell(0, 1) &&
                labelValue("snapshotEntrySha") == cell(0, 3) &&
                labelValue("snapshotEntryTarget") == cell(0, 4), "file details match actual manifest");
        window->findChild<QPushButton *>("snapshotSelectedIdCopy")->click();
        require(QApplication::clipboard()->text() == QString::fromStdString(single.id), "full snapshot ID copy");
        window->findChild<QPushButton *>("snapshotInspectFile")->click();
        auto *entryDetails = window->findChild<QDialog *>("snapshotFileDetails");
        require(entryDetails->isVisible(), "selected manifest entry opens details");
        window->findChild<QPushButton *>("snapshotEntryShaCopy")->click();
        require(QApplication::clipboard()->text() == cell(0, 3), "full manifest hash copy");
        entryDetails->reject();

#ifdef Q_OS_WIN
        target->setText(junction);
        name->setText("blocked-root");
        waitFor(complete);
        require(!restore->isEnabled() && status->text().contains(QString::fromUtf8(u8"重解析点")),
            "reparse point parent accepted");
        require(RemoveDirectoryW(QDir::toNativeSeparators(junction).toStdWString().c_str()), "remove test junction");
        target->setText(temp.path());
        name->setText("empty");
        waitFor(complete);
        require(!restore->isEnabled() && status->text().contains(QString::fromUtf8(u8"已存在")), "existing target accepted");
        const auto longParent = temp.filePath("long parent alias fixture");
        require(QDir().mkdir(longParent), "cannot create short-name fixture");
        wchar_t shortPath[512]{};
        const auto length = GetShortPathNameW(QDir::toNativeSeparators(longParent).toStdWString().c_str(),
            shortPath, 512);
        require(length && length < 512, "short-name fixture query failed");
        const auto alias = QDir::fromNativeSeparators(QString::fromWCharArray(shortPath));
        if (alias.compare(longParent, Qt::CaseInsensitive) != 0) {
            target->setText(alias);
            startRestore("no-alias");
            finished(QString::fromUtf8(u8"恢复失败"));
            require(restoreStatus->text().contains("alias") && !QFile::exists(longParent + "/no-alias"),
                "short-name alias accepted at runtime");
            target->setText(temp.path());
        } else {
            std::cout << "8.3 parent alias skipped (volume did not generate one)\n";
        }
        startRestore("race");
        auto *race = acceptDownload(first);
        require(QDir().mkdir(temp.filePath("race")), "cannot create competing destination");
        writeFile(temp.filePath("race/keep"));
        deliver(race, first, "first");
        finished(QString::fromUtf8(u8"恢复失败"));
        require(!QFile::exists(temp.filePath("race/only")) && readFile(temp.filePath("race/keep")) == "preserve",
                "publish damaged competing destination");
        noStaging();
        startRestore("dropped");
        acceptDownload(first)->abort();
        finished(QString::fromUtf8(u8"恢复失败"));
        require(!QFile::exists(temp.filePath("dropped")), "disconnect published target");
        noStaging();
        startRestore("cancelled");
        auto *transfer = acceptDownload(first);
        restoreCancel->click();
        finished(QString::fromUtf8(u8"恢复已取消"));
        waitFor([&] { return transfer->state() == QAbstractSocket::UnconnectedState; });
        require(!QFile::exists(temp.filePath("cancelled")), "cancel published target");
        noStaging();
        startRestore("only-one");
        deliver(acceptDownload(first), first, "first");
        finished(QString::fromUtf8(u8"恢复成功"));
        require(readFile(temp.filePath("only-one/only")) == "first", "single restore bytes mismatch");
        noStaging();

        startRestore("foreign");
        auto *foreignPeer = acceptDownload(first);
        const auto staging = QDir(temp.path()).entryList({".chunkharbor-restore-*"},
            QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot);
        require(staging.size() == 1, "expected one owned staging directory");
        const auto foreignPath = temp.filePath(staging.front() + "/foreign.txt");
        writeFile(foreignPath);
        deliver(foreignPeer, first, "first");
        finished(QString::fromUtf8(u8"清理失败"));
        require(restoreStatus->text().contains(staging.front()) &&
            readFile(foreignPath) == "preserve" && !QFile::exists(temp.filePath("foreign")),
            "staging with foreign content must be preserved and reported");
        require(QFile::remove(foreignPath) && QDir(temp.path()).rmdir(staging.front()),
            "remove only explicit foreign test fixture and its empty parent");

        name->clear();
        auto *nestedPeer = open(0, nested);
        std::vector<ListedFile> files;
        for (std::uint32_t i = 1; i < nested.fileCount; ++i)
            files.push_back(readyFile("dir/" + std::to_string(1000 + i), "bytes"));
        files.push_back(readyFile(u8"目录/层级/文件.txt", "bytes"));
        cursor.clear();
        for (std::size_t offset = 0; offset < files.size(); offset += MaxSnapshotEntriesPerPage) {
            const auto end = std::min(files.size(), offset + MaxSnapshotEntriesPerPage);
            const auto next = end < files.size() ? files[end - 1].path : "";
            pageReply(nestedPeer, nested, cursor, {files.begin() + offset, files.begin() + end}, next);
            cursor = next;
        }
        waitFor(complete);
        startRestore("bad-chunk");
        deliver(acceptDownload(files[0]), files[0], "bytes");
        deliver(acceptDownload(files[1]), files[1], "bytes", false);
        finished(QString::fromUtf8(u8"恢复失败"));
        require(restoreStatus->text().contains("checksum") && !QFile::exists(temp.filePath("bad-chunk")),
                "corruption or partial staging was not reported");
        noStaging();
        startRestore("bad-whole");
        deliver(acceptDownload(files[0]), files[0], "wrong");
        finished(QString::fromUtf8(u8"恢复失败"));
        require(restoreStatus->text().contains("whole-file checksum") && !QFile::exists(temp.filePath("bad-whole")),
                "whole-file corruption was not rejected");
        noStaging();
        startRestore("nested");
        for (const auto &entry : files) {
            require(!QFile::exists(temp.filePath("nested")), "partial final tree became visible");
            deliver(acceptDownload(entry), entry, "bytes");
        }
        finished(QString::fromUtf8(u8"恢复成功"));
        for (const auto &entry : files)
            require(readFile(temp.filePath("nested/" + QString::fromUtf8(entry.path))) == "bytes",
                "nested restore content mismatch");
        noStaging();
        name->clear();
#endif

        auto *broken = open(1, large);
        pageReply(broken, large, "", {file("short")});
        waitFor([&] { return !cancel->isEnabled(); });
        require(!complete() && model->rowCount() == 0, "premature EOF yielded a complete preview");
        auto *badPath = open(3, single);
        const auto badPull = receive(badPath);
        send(badPath, SnapshotEntriesPageType, badPull.requestId,
             "{\"snapshot_id\":\"" + single.id + "\",\"entries\":[{\"path\":\"../escape\",\"version_id\":\"" +
             std::string(32, 'a') + "\",\"size\":1,\"sha256\":\"" + std::string(64, 'b') +
             "\"}],\"next_after_path\":\"\"}");
        waitFor([&] { return !cancel->isEnabled(); });
        require(!complete() && model->rowCount() == 0, "traversal page exposed a preview");
        auto *wrongId = open(3, single);
        const auto wrongPull = receive(wrongId);
        send(wrongId, SnapshotEntriesPageType, wrongPull.requestId,
             encodeSnapshotEntriesPage({large.id, {file("other")}, {}}));
        waitFor([&] { return !cancel->isEnabled(); });
        require(!complete() && model->rowCount() == 0, "foreign snapshot row exposed a preview");
        auto *dropped = open(1, large);
        pageReply(dropped, large, "", {file("first")}, "first");
        require(receive(dropped).type == ListSnapshotEntriesType, "missing continued pull");
        dropped->abort();
        waitFor([&] { return !cancel->isEnabled(); });
        require(!complete() && model->rowCount() == 0, "disconnect exposed partial manifest");
        auto *cancelled = open(1, large);
        require(receive(cancelled).type == ListSnapshotEntriesType, "missing cancellable pull");
        cancel->click();
        waitFor([&] { return cancelled->state() == QAbstractSocket::UnconnectedState; });
        require(!complete() && model->rowCount() == 0, "cancel exposed partial manifest");

        auto *many = open(1, large);
        QElapsedTimer scale;
        scale.start();
        cursor.clear();
        int pulls = 0;
        for (std::uint32_t offset = 0; offset < large.fileCount; offset += MaxSnapshotEntriesPerPage) {
            const auto end = std::min(large.fileCount, offset + MaxSnapshotEntriesPerPage);
            std::vector<ListedFile> rows;
            for (auto i = offset; i < end; ++i)
                rows.push_back(file(std::to_string(10000 + i) + ".bin"));
            const auto next = end < large.fileCount ? rows.back().path : "";
            pageReply(many, large, cursor, std::move(rows), next);
            cursor = next;
            ++pulls;
        }
        waitFor(complete);
        require(pulls == 1667 && model->rowCount() == 100 && cell(0, 0) == "10000.bin" &&
                status->text().contains("10000"), "10000-item manifest page or totals");
        window->findChild<QPushButton *>("snapshotEntriesNext")->click();
        require(cell(0, 0) == "10100.bin", "local display paging");
        std::cout << "10000 entries in " << pulls << " bounded pulls, loopback elapsed " << scale.elapsed() << " ms\n";
        target->clear();
        target->setText(temp.path());
        require(cancel->isEnabled(), "queued replacement preflight must remain cancellable");
        cancel->click();
        require(!complete() && model->rowCount() == 0, "preflight cancel exposed a preview");
#ifdef Q_OS_WIN
        const auto beginCapture = [&](const QString &input, const std::string &directory,
                                      std::vector<SnapshotInfo> baseline = {}) {
            root->setText(input);
            startCapture();
            auto *socket = hello();
            const auto listRequest = receive(socket);
            const auto request = decodeListSnapshots(listRequest.payload);
            require(listRequest.type == ListSnapshotsType && request.root == directory &&
                    request.limit == 1 && !request.before, "capture must read latest sequence baseline");
            send(socket, SnapshotsPageType, listRequest.requestId,
                encodeSnapshotsPage({directory, std::move(baseline), {}}));
            const auto create = receive(socket);
            require(create.type == CreateSnapshotType && decodeCreateSnapshot(create.payload) == directory &&
                !capture->isEnabled(), "expected one create for the confirmed folder");
            return std::make_pair(socket, create.requestId);
        };
        const auto acknowledge = [&](QTcpSocket *socket, std::uint64_t requestId, const SnapshotInfo &info) {
            send(socket, SnapshotInfoType, requestId, encodeSnapshotInfo(info));
            const auto verify = receive(socket);
            require(verify.type == GetSnapshotType && decodeGetSnapshot(verify.payload) == info.id,
                    "created snapshot must be fixed before reading entries");
            send(socket, SnapshotInfoType, verify.requestId, encodeSnapshotInfo(info));
        };
        root->setText("fixture");
        startCapture(false);
        require(!listener.hasPendingConnections(), "declined capture opened a connection");
        window->show();
        capture->click();
        require(window->findChild<QMessageBox *>(), "capture confirmation missing");
        require(window->close(), "unconfirmed capture should not delay close");
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        require(!window->findChild<QMessageBox *>() && !listener.hasPendingConnections(),
            "closing during confirmation left a live prompt or connection");
        window->show();
        startCapture();
        auto *oldEndpoint = hello();
        require(receive(oldEndpoint).type == ListSnapshotsType, "endpoint-switch baseline missing");
        serverPort->setValue(listener.serverPort() == 65535 ? 65534 : listener.serverPort() + 1);
        serverPort->setValue(listener.serverPort());
        waitFor([&] { return oldEndpoint->state() == QAbstractSocket::UnconnectedState; });
        require(oldEndpoint->bytesAvailable() == 0 && capture->isEnabled(),
            "switching away and back must invalidate an old unsubmitted capture");
        startCapture();
        auto *unsubmitted = hello();
        require(receive(unsubmitted).type == ListSnapshotsType, "capture baseline missing");
        window->show();
        require(window->close(), "read-only baseline should not delay close");
        waitFor([&] { return unsubmitted->state() == QAbstractSocket::UnconnectedState; });
        require(unsubmitted->bytesAvailable() == 0, "closing during baseline sent a create");
        window->show();
        for (const auto *directory : {"empty-root", "missing-root"}) {
            auto [socket, requestId] = beginCapture(directory, directory);
            const SnapshotInfo zero{std::string(32, 'f'), directory, 6, 10, 0};
            acknowledge(socket, requestId, zero);
            pageReply(socket, zero, "", {});
            waitFor(complete);
            name->setText("no-empty-download");
            require(!restore->isEnabled() && !QDir(temp.filePath("no-empty-download")).exists() &&
                status->text().contains(QString::fromUtf8(u8"没有已提交文件")), "empty current folder claimed download");
        }
        name->clear();

        const SnapshotInfo created{std::string(32, '1'), "fixture", 6, 10, 7};
        auto [folder, folderRequest] = beginCapture("fixture/", "fixture", {nested});
        acknowledge(folder, folderRequest, created);
        std::vector<ListedFile> current{files.begin(), files.begin() + 6};
        current.push_back(files.back());
        current.back().versionId = std::string(32, '2');
        pageReply(folder, created, "", {current.begin(), current.begin() + 6}, current[5].path);
        pageReply(folder, created, current[5].path, {current.back()});
        waitFor(complete);
        require(model->rowCount() == 7 && status->text().contains(QString::fromStdString(created.id)),
            "folder download lost complete pinned manifest");
        startRestore("captured");
        for (const auto &entry : current) deliver(acceptDownload(entry), entry, "bytes");
        finished(QString::fromUtf8(u8"恢复成功"));
        require(readFile(temp.filePath(QString::fromUtf8(u8"captured/目录/层级/文件.txt"))) == "bytes" &&
                restoreStatus->text().contains(QString::fromStdString(created.id)), "folder output or progress mismatch");
        noStaging();

        const SnapshotInfo candidate{std::string(32, '3'), "fixture", 7, 10, 1};
        const SnapshotInfo concurrent{std::string(32, '4'), "fixture", 8, 10, 1};
        auto [lost, lostRequest] = beginCapture("fixture", "fixture", {created});
        (void)lostRequest;
        window->show();
        require(!window->close(), "close while create is unresolved must await reconciliation");
        lost->abort(); // The server committed candidate, but its acknowledgement was lost.
        auto *reconnect = hello();
        const auto lookupRequest = receive(reconnect);
        require(lookupRequest.type == ListSnapshotsType && decodeListSnapshots(lookupRequest.payload).root == "fixture",
            "lost create response must trigger a read-only lookup");
        send(reconnect, SnapshotsPageType, lookupRequest.requestId,
            encodeSnapshotsPage({"fixture", {concurrent, candidate, created}, {}}));
        waitFor([&] { return list->rowCount() == 2 && status->text().contains(QString::fromUtf8(u8"无法确认")); });
        require(window->isVisible() && !complete() && list->currentRow() < 0 &&
                !listener.hasPendingConnections(), "ambiguous candidate selected or create retried");

        const SnapshotInfo finalCapture{std::string(32, '5'), "fixture", 9, 10, 1};
        auto [closing, closingRequest] = beginCapture("fixture", "fixture", {concurrent});
        acknowledge(closing, closingRequest, finalCapture);
        pageReply(closing, finalCapture, "", {first});
        waitFor(complete);
        startRestore("closing");
        auto *closingDownload = acceptDownload(first);
        require(!window->close(), "active restore should defer close until cleanup");
        waitFor([&] { return !window->isVisible() && closingDownload->state() == QAbstractSocket::UnconnectedState; });
        require(!QFile::exists(temp.filePath("closing")), "close published a partial directory");
        noStaging();
        window.reset();
#else
        auto *closing = open(3, single);
        require(receive(closing).type == ListSnapshotEntriesType, "missing close-test pull");
        window.reset();
        waitFor([&] { return closing->state() == QAbstractSocket::UnconnectedState; });
#endif
        std::cout << "snapshot empty/single/multipage/Windows conflicts/switch/cancel/disconnect/close passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
