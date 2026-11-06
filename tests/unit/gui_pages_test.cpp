#include "MainWindow.h"
#include "QtTcpTransport.h"
#include "TransferController.h"
#include "FileListController.h"
#include "WorkspaceStore.h"
#include "chunkharbor/protocol/DataChunk.h"
#include "pages/BackupPage.h"
#include "pages/DashboardPage.h"
#include "pages/FilesPage.h"
#include "pages/LogsPage.h"
#include "pages/SettingsPage.h"
#include "pages/VersionsPage.h"
#include <QApplication>
#include <QStackedWidget>
#include <QComboBox>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QTemporaryDir>
#include <QTableView>
#include <QTableWidget>
#include <QTimer>
#include <QScrollArea>
#include <QClipboard>
#include <QSplitter>
#include <iostream>
#include <stdexcept>
void check(bool v, const char *m) {
    if (!v)
        throw std::runtime_error(m);
}
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    try {
        QTemporaryDir temp;
        check(temp.isValid(), "temporary workspace");
        QCoreApplication::setOrganizationName("ChunkHarborTests");
        QCoreApplication::setApplicationName("pages");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temp.path());
        QSettings().setValue("connection/port", 19087);
        {
            SettingsPage settings;
            auto *mode = settings.findChild<QComboBox *>("connectionMode");
            check(mode && mode->currentData() == "ssh" && settings.port->value() == 19087,
                  "legacy SSH settings preserved");
            mode->setCurrentIndex(mode->findData("tcp"));
            check(settings.serverHost().isEmpty() && settings.port->value() == 8787,
                  "direct mode requires an address");
            settings.host->setText("999.1.2.3");
            check(settings.serverHost().isEmpty(), "invalid IPv4 rejected");
            settings.host->setText("VM.Example");
            settings.port->setValue(9878);
            check(settings.backupConfig().direct && settings.backupConfig().host == "vm.example",
                  "backup receives the selected direct endpoint");
            mode->setCurrentIndex(mode->findData("ssh"));
            check(settings.port->value() == 19087 && settings.serverHost() == "127.0.0.1",
                  "SSH profile independent");
            mode->setCurrentIndex(mode->findData("tcp"));
        }
        {
            SettingsPage settings;
            check(settings.backupConfig().direct && settings.serverHost() == "vm.example" &&
                  settings.port->value() == 9878, "direct profile restored after reopening settings");
        }
        QSettings().remove("connection");
        WorkspaceStore store(temp.filePath("data"));
        TransferController controller(&store);
        SettingsPage settings;
        BackupPage backup(&controller, &settings);
        auto *backupMode = backup.findChild<QComboBox *>("backupMode");
        check(backupMode && backupMode->count() == 4 && backupMode->currentData() == "auto",
              "new workspace defaults to auto with all manual modes retained");
        for (const auto &manual : {"full", "resume", "incremental"}) {
            QSettings().setValue("task/mode", manual);
            BackupPage legacy(&controller, &settings);
            check(legacy.findChild<QComboBox *>("backupMode")->currentData() == manual,
                  "saved manual mode remains compatible");
        }
        QSettings().remove("task/mode");
        {
            WorkspaceStore failedStore(temp.filePath("failed-scan"));
            TransferController failedScan(&failedStore);
            gui::BackupConfig config;
            config.directory = temp.filePath("failed-source");
            check(QDir().mkpath(config.directory + "/build") && QDir().mkpath(failedScan.stateDirectory()),
                  "create isolated global-error fixture");
            QFile bad(failedScan.stateDirectory() + "/unassociated.json");
            check(bad.open(QIODevice::WriteOnly), "create malformed unassociated checkpoint");
            bad.write("{broken"); bad.close();
            QEventLoop loop;
            QObject::connect(&failedScan, &TransferController::finished, &loop, &QEventLoop::quit);
            QTimer::singleShot(3000, &loop, &QEventLoop::quit);
            failedScan.scan(config);
            loop.exec();
            check(!failedScan.busy() && !failedScan.hasPreview() && failedScan.recommendation().contains("核验失败"),
                  "global failure with excluded rows must not advertise a confirmed auto plan");
        }
        check(controller.previewModel()->rowCount() == 0, "no fake preview entries");
        auto *start = backup.findChild<QPushButton *>("backupStart");
        check(start && !start->isEnabled(), "no upload before a preview");
        settings.port->setValue(23456);
        SettingsPage reopened;
        check(reopened.port->value() == 23456, "persist endpoint setting");
        check(settings.backupConfig().chunkSize == 262144, "valid default chunk");
        auto *chunks = settings.findChild<QComboBox *>("defaultChunkSize");
        for (int i = 0; i < chunks->count(); ++i)
            check(chunks->itemData(i).toUInt() <= chunkharbor::protocol::MaxChunkDataSize,
                  "all offered chunk sizes fit wire payload including chunk header");
        QtTcpTransport transport;
        DashboardPage dashboard(&controller, &store, &transport);
        auto *connection = dashboard.findChild<QLabel *>("dashboardMetric0");
        check(connection->text() == "未检测", "dashboard does not invent a connection result");
        FileListController fileList;
        FilesPage files(&fileList);
        VersionsPage versions(&store);
        LogsPage logs(&store);
        auto *fileRows = files.findChild<QTableView *>("filesTable")->model();
        auto *list = versions.findChild<QListWidget *>("versionFiles");
        check(fileRows->rowCount() == 0 && list->count() == 0, "empty real histories");
        auto *download = files.findChild<QPushButton *>("filesRestore");
        check(download && !download->isEnabled(), "download requires a selection");
        fileList.model()->setPage({{{"中文/a.txt", std::string(32, '1'), 123, std::string(64, 'a')}}, {}});
        files.findChild<QTableView *>("filesTable")->selectRow(0);
        check(download->isEnabled(), "selected server file must allow current-version download");
        check(files.findChild<QLabel *>("fileDetails")->property("fullText").toString() == "中文/a.txt",
              "detail uses actual selected logical path");
        files.findChild<QPushButton *>("fileShaCopy")->click();
        check(QApplication::clipboard()->text() == QString(64, 'a'), "copy retains full hash");
        files.findChild<QLineEdit *>("fileSearch")->setText("not-present");
        check(!download->isEnabled() && files.findChild<QLabel *>("fileDetails")->property("fullText").toString().isEmpty(),
              "filtering out the selection clears details and download action");
        files.findChild<QLineEdit *>("fileSearch")->clear();
        fileList.model()->setPage({});
        check(!download->isEnabled(), "reset page invalidates download selection");
        gui::FileRecord r;
        r.localPath = temp.filePath("source.txt");
        r.logicalPath = "工作区/source.txt";
        r.status = "已完成";
        r.change = "新增";
        r.size = 123;
        r.changedBlocks = 1;
        r.sha256 = QString(64, 'a');
        r.versionId = QString(32, '1');
        r.baseVersion = "absent";
        r.endpoint = "127.0.0.1:23456";
        r.time = "2026-11-02T01:00:00Z";
        store.addCommit(r);
        auto other = r;
        other.versionId = QString(32, '2');
        other.sha256 = QString(64, 'b');
        other.time = "2026-11-02T02:00:00Z";
        store.addCommit(other);
        check(fileRows->rowCount() == 0 && list->count() == 1,
              "server files must not be populated by local commit history");
        check(versions.findChild<QStandardItemModel *>("versionHistoryModel")->rowCount() == 2,
              "actual versions");
        check(versions.findChild<QLabel *>("versionSha")->property("fullText").toString() == other.sha256,
              "latest actual SHA");
        auto *historyList = versions.findChild<QListView *>("versionHistory");
        check(historyList, "version history uses a compact list");
        historyList->setCurrentIndex(historyList->model()->index(1, 0));
        check(versions.findChild<QLabel *>("versionId")->property("fullText").toString() == r.versionId,
              "selecting an older version binds that exact record");
        versions.findChild<QPushButton *>("versionIdCopy")->click();
        check(QApplication::clipboard()->text() == r.versionId, "copy full selected version ID");
        auto *search = files.findChild<QLineEdit *>("fileSearch");
        search->setText("absent-file");
        check(fileRows->rowCount() == 0, "file search");
        search->clear();
        check(fileRows->rowCount() == 0, "clear file search does not invent server rows");
        store.appendLog("信息", "真实文件校验完成");
        store.appendLog("警告", "真实连接中断");
        auto *logRows = logs.findChild<QStandardItemModel *>("logModel");
        check(logRows->rowCount() == 2, "live actual log entries");
        logs.findChild<QComboBox *>("logLevel")->setCurrentIndex(2);
        check(logRows->rowCount() == 1 && logRows->item(0, 0)->text() == "警告",
              "live level filter");
        logs.findChild<QLineEdit *>("logSearch")->setText("校验");
        check(logRows->rowCount() == 0, "combined filter");
        controller.start(settings.backupConfig());
        check(!controller.busy(), "invalid start rejected before thread/network");
        check(dashboard.findChild<QLabel *>("dashboardMetric3")->text().contains("2"),
              "dashboard actual version count");
        QSettings().setValue("storage/logDirectory", temp.filePath("window-logs"));
        MainWindow window(nullptr, temp.filePath("window"), MainWindow::StartupMode::Manual);
        check(window.findChild<QLineEdit *>("snapshotRoot"), "snapshot browser entry is available");
        const bool dashboardLayoutOnly = app.arguments().contains("--dashboard-layout");
        window.findChild<QListWidget *>("navigation")->setCurrentRow(dashboardLayoutOnly ? 0 : 2);
        window.show();
        QApplication::processEvents();
        QEventLoop maximizeLoop;
        if (!dashboardLayoutOnly) {
            auto *filesTable = window.findChild<QTableView *>("filesTable");
            check(filesTable->isColumnHidden(2) && filesTable->isColumnHidden(3),
                  "version and hash belong in the details panel");
            for (auto *ancestor = filesTable->parentWidget(); ancestor; ancestor = ancestor->parentWidget())
                check(!qobject_cast<QScrollArea *>(ancestor), "file table must not nest inside a page scroll area");
            check(filesTable->viewport()->height() >= 7 * 42, "normal window shows at least seven file rows");
            const int normalHeight = filesTable->height();
            window.resize(1440, 1100);
            QApplication::processEvents();
            check(filesTable->height() >= normalHeight + 200, "extra height expands the file table");
            auto *windowFiles = window.fileListController();
            chunkharbor::protocol::FilesResult layoutPage;
            for (int i = 0; i < 32; ++i)
                layoutPage.files.push_back({"中文/" + std::string(140, 'x') + (i < 10 ? "0" : "") + std::to_string(i) + ".jpg",
                                           std::string(32, '1'), 8710908, std::string(64, 'a')});
            windowFiles->model()->setPage(std::move(layoutPage));
            filesTable->selectRow(0);
            window.resize(1060, 700);
            QApplication::processEvents();
            auto *detailsPanel = window.findChild<QWidget *>("filesDetailsPanel");
            check(detailsPanel->width() >= 280 && filesTable->width() >= 320, "narrow window respects panel minimum widths");
            check(filesTable->model()->index(0, 1).data().toString() == "8.3 MB", "friendly sizes are presentation only");
            auto *toggle = window.findChild<QPushButton *>("filesToggleDetails");
            const int expandedWidth = filesTable->width();
            toggle->click();
            QApplication::processEvents();
            check(detailsPanel->isHidden() && filesTable->width() > expandedWidth + 200, "details collapse returns space to list");
            toggle->click();
            window.showMaximized();
            QTimer::singleShot(100, &maximizeLoop, &QEventLoop::quit);
            maximizeLoop.exec();
            std::cerr << "maximized: window=" << window.width() << 'x' << window.height()
                      << " viewport=" << filesTable->viewport()->height()
                      << " details=" << detailsPanel->isVisible() << '\n';
            // The offscreen virtual screen can be smaller than the supported minimum
            // after scaling; assert full row capacity only at supported window sizes.
            check(filesTable->viewport()->height() >= (window.height() >= 700 ? 7 * 42 : 1) && detailsPanel->isVisible(),
                  "maximized window keeps file rows and details usable");
            window.showNormal();
            window.resize(1280, 820);
            QApplication::processEvents();
            if (argc > 1) window.grab().save(QString::fromLocal8Bit(argv[1]));
        }
        window.findChild<QListWidget *>("navigation")->setCurrentRow(0);
        QApplication::processEvents();
        check(window.findChild<QWidget *>("filesTransferBar")->isVisible(), "transfer bar remains visible across navigation");
        auto *badge = window.findChild<QLabel *>("connectionBadge");
        check(badge->text() == "未检测", "topbar starts unchecked");
        window.findChild<QSpinBox *>("serverPort")->setValue(34567);
        check(badge->text() == "未检测", "endpoint edit requires a new check");
        auto *recent = window.findChild<QTableWidget *>("dashboardRecentBackups");
        auto *activity = window.findChild<QTableWidget *>("dashboardRecentActivity");
        check(recent && recent->rowCount() == 0, "dashboard empty history has no placeholder rows");
        for (int i = 0; i < 6; ++i) {
            auto record = r;
            record.logicalPath = QString("中文归档/备份文件-%1-").arg(i) + QString(100, QChar(0x957f)) + ".txt";
            record.versionId = QString(32, QChar('1' + i));
            record.time = QString("2026-11-03T12:00:0%1Z").arg(i);
            window.store()->addCommit(record);
            window.store()->appendLog("信息", QString("测试活动 %1").arg(i));
        }
        check(recent->rowCount() == 5 && recent->item(0, 1)->text().startsWith("中文归档/备份文件-5-"),
              "latest real backups populate capped rows in time order");
        check(recent->item(0, 1)->toolTip().contains(recent->item(0, 1)->text()), "full path stays available");
        check(activity->rowCount() == 5 && activity->item(0, 1)->text() == "测试活动 5", "activity uses actual log");
        QApplication::processEvents();
        check(recent->columnWidth(0) >= recent->fontMetrics().horizontalAdvance(recent->item(0, 0)->text()) + 12,
              "time column fits formatted date and time");
        std::cout << "dashboard device pixel ratio=" << window.devicePixelRatioF() << '\n';
        if (argc > 2) check(window.grab().save(QString::fromLocal8Bit(argv[2])), "dashboard screenshot");
        auto *detect = window.findChild<QPushButton *>("detectConnection");
        auto *address = window.findChild<QLabel *>("connectionAddress");
        QLineEdit *host = nullptr;
        for (auto *edit : window.findChildren<QLineEdit *>()) {
            if (edit->isReadOnly() && edit->text() == "127.0.0.1") host = edit;
        }
        check(host != nullptr, "find configured loopback address");
        host->setText("2001:0db8:1234:5678:9abc:def0:1234:5678");
        window.resize(1060, 700);
        QApplication::processEvents();
        check(address->toolTip().contains("[2001:0db8:1234:5678:9abc:def0:1234:5678]:34567"),
              "configured IPv6 address remains fully inspectable");
        check(address->geometry().right() < detect->geometry().left() && detect->isVisible() && detect->isEnabled(),
              "long address never crowds out detection");
        check(recent->parentWidget()->height() == activity->parentWidget()->height(), "recent panels are equal height");
        const int cardWidth = window.findChild<QLabel *>("dashboardMetric0")->parentWidget()->width();
        const int cardHeight = window.findChild<QLabel *>("dashboardMetric0")->parentWidget()->height();
        for (int i = 1; i < 4; ++i) {
            auto *card = window.findChild<QLabel *>(QString("dashboardMetric%1").arg(i))->parentWidget();
            check(qAbs(card->width() - cardWidth) <= 2 && card->height() == cardHeight, "dashboard cards share geometry");
        }
        const int compactHeight = recent->height();
        window.resize(1440, 1100);
        QApplication::processEvents();
        check(recent->height() > compactHeight + 200, "recent list absorbs extra height");
        window.showMaximized();
        QTimer::singleShot(100, &maximizeLoop, &QEventLoop::quit);
        maximizeLoop.exec();
        check(recent->isVisible() && detect->isVisible(), "dashboard controls stay available when maximized");
        window.findChild<QPushButton *>("topbarSettings")->click();
        check(window.findChild<QListWidget *>("navigation")->currentRow() == 6, "settings action retains navigation");
        auto *navigation = window.findChild<QListWidget *>("navigation");
        auto *pageStack = window.findChild<QStackedWidget *>("pageStack");
        const QStringList labels{"总览", "备份", "文件", "版本历史", "目录快照", "日志", "设置"};
        const QStringList markers{"dashboardMetric0", "backupStart", "filesTable", "versionsSplitter",
                                  "snapshotRoot", "logSearch", "serverPort"};
        check(navigation->count() == labels.size(), "navigation preserves every page");
        for (int row = 0; row < labels.size(); ++row) {
            navigation->setCurrentRow(row);
            check(navigation->item(row)->text() == labels[row] &&
                  pageStack->currentWidget()->findChild<QWidget *>(markers[row]),
                  "navigation order matches its actual page");
        }

        std::cout << "real empty states, settings persistence, history, logs and connection "
                     "binding passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
