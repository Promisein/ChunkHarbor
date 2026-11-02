#include "MainWindow.h"
#include "TaskModel.h"
#include "pages/BackupPage.h"
#include <QApplication>
#include <QEventLoop>
#include <QLabel>
#include <QListWidget>
#include <QScrollBar>
#include <QSettings>
#include <QTableView>
#include <QTemporaryDir>
#include <QTimer>
#include <QVBoxLayout>
#include <iostream>
#include <stdexcept>

namespace {
void check(bool condition, const QString &message) {
    if (!condition)
        throw std::runtime_error(message.toStdString());
}

void settle() {
    QEventLoop loop;
    QTimer::singleShot(100, &loop, &QEventLoop::quit);
    loop.exec();
}

void checkPreview(QTableView *table, const QString &state) {
    auto *card = table->parentWidget();
    auto *layout = qobject_cast<QVBoxLayout *>(card->layout());
    check(layout && layout->count() == 3, state + ": preview title, summary and table exist");
    auto *title = qobject_cast<QLabel *>(layout->itemAt(0)->widget());
    auto *summary = qobject_cast<QLabel *>(layout->itemAt(1)->widget());
    check(title && summary, state + ": preview labels exist");
    const int titleGap = summary->y() - title->geometry().bottom() - 1;
    const int tableGap = table->y() - summary->geometry().bottom() - 1;
    std::cout << state.toStdString() << ": card=" << card->height()
              << " table=" << table->height() << " gaps=" << titleGap << ',' << tableGap
              << std::endl;
    check(titleGap >= 0 && titleGap <= layout->spacing() + 2,
          state + ": title-to-summary spacing must stay compact");
    check(tableGap >= 0 && tableGap <= layout->spacing() + 2,
          state + ": summary-to-table spacing must stay compact");
    check(title->height() <= title->sizeHint().height() + 2 &&
              summary->height() <= summary->sizeHint().height() + 2,
          state + ": preview labels must not absorb the table's usable height");
    const int bottomGap = card->height() - table->geometry().bottom() - 1;
    check(bottomGap <= layout->contentsMargins().bottom() + 2,
          state + ": table must fill the remaining preview card height");
}
} // namespace

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QApplication::setStyle("Fusion");
    try {
        QTemporaryDir temp;
        check(temp.isValid(), "isolated layout test workspace");
        QCoreApplication::setOrganizationName("ChunkHarborTests");
        QCoreApplication::setApplicationName("backup-layout");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temp.path());
        QSettings().setValue("storage/logDirectory", temp.filePath("logs"));
        MainWindow window(nullptr, temp.filePath("data"));
        window.findChild<QListWidget *>("navigation")->setCurrentRow(1);
        auto *table = window.findChild<QTableView *>("backupTable");
        check(table, "backup preview table exists");
        auto *model = qobject_cast<TaskModel *>(table->model());
        check(model, "real backup preview model exists");
        for (int rows : {0, 3, 200}) {
            model->clear();
            for (int i = 0; i < rows; ++i) {
                gui::FileRecord record;
                record.logicalPath = QString("layout/file-%1.txt").arg(i);
                record.change = QString::fromUtf8("新增");
                record.status = QString::fromUtf8("待上传");
                record.size = 123;
                model->upsert(record);
            }
            const QString prefix = QString("rows=%1 ").arg(rows);
            window.showNormal();
            window.resize(1280, 820);
            settle();
            checkPreview(table, prefix + "normal");
            const int normalHeight = table->height();
            window.resize(1440, 1100);
            settle();
            checkPreview(table, prefix + "large resize");
            check(table->height() > normalHeight + 150,
                  prefix + "extra window height must become usable table height");
            window.showMaximized();
            settle();
            checkPreview(table, prefix + "maximized");
            window.showNormal();
            settle();
            checkPreview(table, prefix + "restored");
            window.resize(1060, 700);
            settle();
            checkPreview(table, prefix + "small resize");
            if (rows == 200) {
                check(table->verticalScrollBar()->maximum() > 0,
                      "large preview remains scrollable in a small window");
                table->scrollToBottom();
                settle();
                check(table->visualRect(model->index(rows - 1, 0)).intersects(table->viewport()->rect()),
                      "last preview row remains reachable");
            }
        }
        std::cout << "backup layout: empty, short and long models across window states passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
