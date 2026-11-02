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
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QTemporaryDir>
#include <QTableView>
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
        WorkspaceStore store(temp.filePath("data"));
        TransferController controller(&store);
        SettingsPage settings;
        BackupPage backup(&controller, &settings);
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
        transport.ready();
        auto *connection = dashboard.findChild<QLabel *>("dashboardMetric0");
        check(connection->text() == "在线", "dashboard follows real transport signal");
        transport.failed();
        check(connection->text() == "离线", "dashboard failure state");
        FileListController fileList;
        FilesPage files(&fileList);
        VersionsPage versions(&store);
        LogsPage logs(&store);
        auto *fileRows = files.findChild<QTableView *>("filesTable")->model();
        auto *list = versions.findChild<QListWidget *>("versionFiles");
        check(fileRows->rowCount() == 0 && list->count() == 0, "empty real histories");
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
        check(versions.findChild<QLabel *>("versionSha")->text() == other.sha256,
              "latest actual SHA");
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
        MainWindow window(nullptr, temp.filePath("window"));
        window.transport()->ready();
        auto *badge = window.findChild<QLabel *>("connectionBadge");
        check(badge->text() == "在线", "topbar connected");
        window.findChild<QSpinBox *>("serverPort")->setValue(34567);
        check(badge->text() == "离线", "endpoint edit invalidates old online state");
        std::cout << "real empty states, settings persistence, history, logs and connection "
                     "binding passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
