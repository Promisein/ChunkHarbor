#include "MainWindow.h"
#include "WorkspaceStore.h"
#include "pages/VersionsPage.h"
#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QEventLoop>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSplitter>
#include <QStandardItemModel>
#include <QTableView>
#include <QTemporaryDir>
#include <QTimer>
#include <iostream>
#include <stdexcept>

void check(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
void settle() {
    QEventLoop loop;
    QTimer::singleShot(100, &loop, &QEventLoop::quit);
    loop.exec();
}
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    try {
        QTemporaryDir temp;
        check(temp.isValid(), "isolated workspace");
        QCoreApplication::setOrganizationName("ChunkHarborTests");
        QCoreApplication::setApplicationName("versions-page");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temp.path());
        QSettings().setValue("storage/logDirectory", temp.filePath("logs"));
        MainWindow window(nullptr, temp.filePath("data"));
        window.findChild<QListWidget *>("navigation")->setCurrentRow(3);
        auto *files = window.findChild<QListWidget *>("versionFiles");
        auto *history = window.findChild<QListView *>("versionHistory");
        auto *model = window.findChild<QStandardItemModel *>("versionHistoryModel");
        auto full = [&](const char *name) { return window.findChild<QLabel *>(name)->property("fullText").toString(); };
        auto *store = window.store();
        check(files->count() == 0 && model->rowCount() == 0, "empty local history has no fake records");
        check(!window.findChild<QPushButton *>("versionIdCopy")->isEnabled(), "empty details cannot copy a fake ID");
        gui::FileRecord older;
        older.logicalPath = "中文/" + QString(180, QChar(0x957f)) + ".txt";
        older.endpoint = "127.0.0.1:18787";
        older.versionId = QString(32, '1');
        older.sha256 = QString(64, 'a');
        older.baseVersion = "absent";
        older.time = "2026-11-03T20:56:00+09:00";
        older.change = "新增";
        older.size = 11469;
        older.changedBlocks = 3;
        older.missingBytes = 10001;
        store->addCommit(older);
        auto newer = older;
        newer.versionId = QString(32, '2');
        newer.baseVersion = older.versionId;
        newer.sha256 = QString(64, 'b');
        newer.change = "已修改";
        newer.time = "2026-11-03T12:00:00.900Z";
        newer.changedBlocks = 1;
        newer.missingBytes = 77;
        store->addCommit(newer);
        check(files->count() == 1 && model->rowCount() == 2, "same endpoint and path group into a single file");
        check(full("versionId") == newer.versionId && full("versionSha") == newer.sha256,
              "latest instant sorts first even when raw offset text sorts differently");
        check(full("versionTime") == QDateTime::fromString(newer.time, Qt::ISODateWithMs).toLocalTime().toString("yyyy-MM-dd HH:mm"),
              "UTC time converts to system local time");
        check(model->index(0, 1).data().toString() == newer.time, "original UTC timestamp remains in model");
        check(model->index(0, 0).data(Qt::UserRole + 2).value<gui::FileRecord>().missingBytes == 77,
              "full change metadata survives compact presentation");
        window.findChild<QPushButton *>("versionBaseCopy")->click();
        check(QApplication::clipboard()->text() == older.versionId, "copy exact base ID");
        history->setCurrentIndex(model->index(1, 0));
        check(full("versionId") == older.versionId && full("versionSha") == older.sha256,
              "older selection displays older content hash and ID");
        check(full("versionComparison").contains("3"), "changed blocks follow the selected version");
        check(!window.findChild<QPushButton *>("versionBaseCopy")->isEnabled(), "absent base is not an ID");
        window.findChild<QPushButton *>("versionIdCopy")->click();
        check(QApplication::clipboard()->text() == older.versionId, "copy exact selected version");
        auto other = older;
        other.endpoint = "127.0.0.1:23456";
        other.versionId = QString(32, '3');
        other.sha256 = QString(64, 'c');
        store->addCommit(other);
        check(files->count() == 2, "same path on another endpoint remains separate");
        files->setCurrentRow(0);
        check(model->rowCount() == 1 && full("versionId") == other.versionId,
              "switching files clears previous history and details");
        history->selectionModel()->clearCurrentIndex();
        check(!window.findChild<QPushButton *>("versionIdCopy")->isEnabled() && full("versionSha").isEmpty(),
              "invalid version selection clears stale metadata");
        auto *search = window.findChild<QLineEdit *>("versionFileSearch");
        search->setText("no-match");
        check(model->rowCount() == 0 && full("versionSha").isEmpty(), "no-match filter clears versions and details");
        search->clear();
        check(model->rowCount() > 0, "clearing local filter restores real records");
        window.show();
        window.resize(1280, 820);
        settle();
        check(history->height() >= 350 && files->height() >= 350, "lists use most normal-window height");
        const int normalHeight = history->height();
        window.resize(1440, 1100);
        settle();
        check(history->height() > normalHeight + 200, "window growth expands history list");
        for (auto *view : {static_cast<QAbstractItemView *>(files), static_cast<QAbstractItemView *>(history)}) {
            check(!view->horizontalScrollBar()->isVisible(), "long values do not create horizontal scrollbars");
            for (auto *ancestor = view->parentWidget(); ancestor; ancestor = ancestor->parentWidget())
                check(!qobject_cast<QScrollArea *>(ancestor), "no outer page scroll area");
        }
        window.resize(1060, 700);
        settle();
        check(window.findChild<QSplitter *>("versionsSplitter")->width() <= window.centralWidget()->width(), "splitter fits narrow window");
        auto *details = window.findChild<QWidget *>("versionDetailsPanel");
        auto *toggle = window.findChild<QPushButton *>("versionsToggleDetails");
        const int width = history->width();
        toggle->click(); settle();
        check(details->isHidden() && history->width() > width + 200, "collapse details returns width to versions");
        toggle->click();
        window.showMaximized(); settle();
        check(history->viewport()->height() > 0, "maximized history stays usable");
        window.showNormal(); window.resize(1280, 820); settle();
        files->setCurrentRow(1);
        settle();
        if (argc > 1) check(window.grab().save(QString::fromLocal8Bit(argv[1])), "save native layout snapshot");
        auto *fileTable = window.findChild<QTableView *>("filesTable");
        check(fileTable->property("visualRole") == history->property("visualRole"), "both pages share browser styling");
        WorkspaceStore reopened(temp.filePath("data"));
        check(reopened.records().size() == 3 && reopened.records().front().time == other.time,
              "UI does not rewrite persisted timestamps or history semantics");
        std::cout << "local history selection, UTC/offset conversion, copy, empty states and layout passed\n";
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
