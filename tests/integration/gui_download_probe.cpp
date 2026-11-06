#include "MainWindow.h"
#include "DownloadController.h"
#include "FileListController.h"
#include <QApplication>
#include <QFileDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QProgressBar>
#include <QSettings>
#include <QSpinBox>
#include <QTableView>
#include <QTableWidget>
#include <QLineEdit>
#include <QTemporaryDir>
#include <QTimer>
#include <iostream>
#include <memory>

int main(int argc, char **argv) {
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    const auto args = app.arguments();
    app.setQuitOnLastWindowClosed(false);
    if (argc < 8) return 64;
    QTemporaryDir temp;
    QCoreApplication::setOrganizationName("ChunkHarborTests");
    QCoreApplication::setApplicationName("download-probe");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temp.path());
    const auto directHost = qEnvironmentVariable("CHUNKHARBOR_TEST_TCP_HOST");
    QSettings().setValue("connection/mode", "tcp");
    QSettings().setValue("connection/tcpHost", directHost.isEmpty() ? "127.0.0.1" : directHost);
    QSettings().setValue("connection/tcpPort", args[1].toUInt());
    if (!directHost.isEmpty()) {
        QSettings().setValue("connection/mode", "tcp");
        QSettings().setValue("connection/tcpHost", directHost);
    }
    QSettings().setValue("storage/logDirectory", temp.filePath("logs"));
    auto window = std::make_unique<MainWindow>(nullptr, temp.filePath("workspace"), MainWindow::StartupMode::Manual);
    const auto path = args[2];
    const auto target = args[6];
    const bool historical = args[7].startsWith("history_");
    const auto mode = historical ? args[7].mid(8) : args[7];
    auto *download = window->downloadController();
    auto *files = window->fileListController();
    const auto port = args[1].toUShort();
    window->findChild<QSpinBox *>("serverPort")->setValue(port);
    window->findChild<QListWidget *>("navigation")->setCurrentRow(historical ? 3 : 2);
    int beats = 0;
    bool duplicateRejected = false;
    bool transferCancelSent = false;
    QTimer pulse;
    pulse.setInterval(10);
    QObject::connect(&pulse, &QTimer::timeout, &app, [&] { ++beats; });
    pulse.start();
    QObject::connect(download, &DownloadController::changed, &app, [&] {
        if (mode == "cancel_transfer" && download->received() && !transferCancelSent) {
            transferCancelSent = true;
            download->cancel();
        }
    });
    auto output = [&](bool success, QString status, quint64 received) {
        if (window && args.size() > 8) window->grab().save(args[8]);
        QJsonObject result{{"success", success}, {"status", status},
            {"received", QString::number(received)}, {"heartbeat", beats},
            {"duplicate_rejected", duplicateRejected}};
        std::cout << "RESULT=" << QJsonDocument(result).toJson(QJsonDocument::Compact).constData() << std::endl;
        app.exit(0);
    };
    QObject::connect(download, &DownloadController::finished, &app, [&](bool success) {
        if (success && window->findChild<QProgressBar *>("filesDownloadProgress")->value() != 100) {
            app.exit(70); return;
        }
        output(success, download->status(), download->received());
    });
    auto start = [&] {
        auto *table = window->findChild<QTableView *>("filesTable");
        QPushButton *button = nullptr;
        if (historical) {
            auto *rows = window->findChild<QTableWidget *>("serverHistoryRows");
            for (int row = 0; row < rows->rowCount(); ++row)
                if (rows->item(row, 1)->text() == args[3]) rows->setCurrentCell(row, 0);
            button = window->findChild<QPushButton *>("serverHistoryRestore");
        } else {
            int selected = -1;
            for (int row = 0; row < table->model()->rowCount(); ++row)
                if (table->model()->index(row, 0).data().toString() == path) selected = row;
            if (selected < 0) { app.exit(65); return; }
            table->selectRow(selected);
            button = window->findChild<QPushButton *>("filesRestore");
        }
        if (!button->isEnabled()) { app.exit(66); return; }
        QTimer::singleShot(30, &app, [&] {
            for (auto *widget : QApplication::topLevelWidgets())
                if (auto *dialog = qobject_cast<QFileDialog *>(widget)) {
                    dialog->selectFile(target);
                    QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection);
                }
        });
        button->click();
        if (download->busy()) {
            duplicateRejected = !download->start("127.0.0.1", port,
                {path.toUtf8().toStdString(), args[3].toStdString(), args[4].toULongLong(), args[5].toStdString()}, target);
            if (historical) {
                auto *version = window->findChild<QLabel *>("filesDownloadVersion");
                if (!version->isVisible() || !version->text().contains(args[3])) { app.exit(72); return; }
            }
            if (button->isEnabled()) { app.exit(68); return; }
            if (mode == "selection_switch") {
                if (historical) {
                    window->findChild<QLineEdit *>("serverHistoryPath")->setText(path + ".other");
                } else {
                    auto page = files->model()->page();
                    auto other = page.files.front();
                    other.path += ".other";
                    page.files.push_back(other);
                    files->model()->setPage(std::move(page));
                    table->selectRow(1);
                }
                window->findChild<QListWidget *>("navigation")->setCurrentRow(0);
                auto *bar = window->findChild<QWidget *>("filesTransferBar");
                auto *status = window->findChild<QLabel *>("filesDownloadStatus");
                if (!bar->isVisible() || download->fileName() != path ||
                    !status->property("fullText").toString().contains(path)) { app.exit(71); return; }
            }
        }
        if (mode == "cancel" || mode == "close" || mode == "destroy")
            QTimer::singleShot(120, &app, [&] {
                if (mode == "cancel") download->cancel();
                else if (mode == "close") window->close();
                else {
                    window.reset();
                    QTimer::singleShot(150, &app, [&] { output(false, "destroyed", 0); });
                }
            });
    };
    window->show();
    QTimer historyReady;
    if (historical) {
        window->findChild<QPushButton *>("versionSource")->click();
        window->findChild<QLineEdit *>("serverHistoryPath")->setText(path);
        QObject::connect(&historyReady, &QTimer::timeout, &app, [&] {
            if (window->findChild<QTableWidget *>("serverHistoryRows")->rowCount() == 0) return;
            historyReady.stop();
            start();
        });
        historyReady.start(10);
        window->findChild<QPushButton *>("serverHistoryFirst")->click();
    } else if (mode == "remote") {
        QObject::connect(files, &FileListController::finished, &app, [&](bool ok) {
            if (ok) start(); else app.exit(69);
        });
        QTimer::singleShot(0, &app, [&] { files->refresh(); });
    } else {
        files->model()->setPage({{{path.toUtf8().toStdString(), args[3].toStdString(),
            args[4].toULongLong(), args[5].toStdString()}}, {}});
        QTimer::singleShot(0, &app, start);
    }
    QTimer::singleShot(18000, &app, [&] { app.exit(67); });
    return app.exec();
}
