#include "MainWindow.h"
#include "TransferController.h"
#include "WorkspaceStore.h"
#include <QApplication>
#include <QComboBox>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QLineEdit>
#include <QPalette>
#include <QSettings>
#include <QTimer>
#include <iostream>
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QApplication::setStyle("Fusion");
    QPalette palette;
    palette.setColor(QPalette::Window, QColor("#f6f7f9"));
    palette.setColor(QPalette::WindowText, QColor("#252a34"));
    palette.setColor(QPalette::Base, Qt::white);
    palette.setColor(QPalette::Text, QColor("#252a34"));
    palette.setColor(QPalette::Button, Qt::white);
    palette.setColor(QPalette::ButtonText, QColor("#252a34"));
    palette.setColor(QPalette::Highlight, QColor("#dfeafb"));
    palette.setColor(QPalette::HighlightedText, QColor("#2167ba"));
    app.setPalette(palette);
    if (argc < 7)
        return 64;
    const auto args = app.arguments();
    QCoreApplication::setOrganizationName("ChunkHarborTests");
    QCoreApplication::setApplicationName("gui-transfer-probe");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, args[1] + "/settings");
    const auto directHost = qEnvironmentVariable("CHUNKHARBOR_TEST_TCP_HOST");
    if (!directHost.isEmpty()) {
        QSettings().setValue("connection/mode", "tcp");
        QSettings().setValue("connection/tcpHost", directHost);
    }
    QSettings().setValue("storage/logDirectory", args[1] + "/logs");
    QSettings().setValue("task/directory", args[2]);
    QSettings().setValue("task/prefix", args[4]);
    QSettings().setValue("task/mode", args[6]);
    MainWindow window(nullptr, args[1]);
    window.findChild<QListWidget *>("navigation")->setCurrentRow(1);
    auto *c = window.controller();
    gui::BackupConfig config;
    config.directory = args[2];
    config.port = args[3].toUShort();
    if (!directHost.isEmpty()) {
        config.host = directHost;
        config.direct = true;
    }
    config.prefix = args[4];
    config.chunkSize = 32768;
    config.mode = args[6];
    config.resume = !args.contains("--no-auto-resume");
    QString operation = args[5];
    const auto originalDirectory = config.directory;
    int switchScans = 0;
    bool uploading = false, interrupted = false, retried = false;
    int heartbeat = 0;
    QTimer beat;
    beat.setInterval(10);
    QObject::connect(&beat, &QTimer::timeout, &app, [&] { ++heartbeat; });
    beat.start();
    QObject::connect(c, &TransferController::progressChanged, &app,
                     [&](const gui::TransferProgress &p) {
                         if (uploading && p.acknowledgedBytes) {
                             std::cout << "ACK=" << p.acknowledgedBytes << '\n' << std::flush;
                             if ((operation == "cancel" || operation == "cancel-large" || operation == "retry-preview") && !interrupted &&
                                 ((operation != "cancel-large" && operation != "retry-preview") || p.acknowledgedBytes >= 32768)) {
                                 interrupted = true;
                                 c->pauseOrResume();
                                 QTimer::singleShot(150, &app, [&] { c->cancel(); });
                             }
                             if (operation == "close" && !interrupted) {
                                 interrupted = true;
                                 window.close();
                             }
                         }
                     });
    QObject::connect(c, &TransferController::finished, &app, [&](const QString &summary, bool ok) {
        if (uploading && !ok && operation == "retry-preview" && !retried) {
            retried = true;
            QTimer::singleShot(0, c, [&] { c->retry(); });
            return;
        }
        if (!uploading && ok && (operation == "mode-change" || operation == "prefix-change" ||
                                 operation == "directory-change")) {
            uploading = true;
            if (operation == "mode-change") {
                config.mode = config.mode == "full" ? "incremental" : "full";
                auto *mode = window.findChild<QComboBox *>("backupMode");
                mode->setCurrentIndex(mode->findData(config.mode));
            } else if (operation == "prefix-change") {
                config.prefix += "-changed";
                window.findChild<QLineEdit *>("backupPrefix")->setText(config.prefix);
            } else {
                config.directory += "/changed";
                window.findChild<QLineEdit *>("backupDirectory")->setText(config.directory);
            }
            if (c->hasPreview()) { app.exit(66); return; }
            QTimer::singleShot(0, c, [&] { c->start(config); });
            return;
        }
        if (operation == "switch-preview" && ok && switchScans < 2 && args.size() > 7) {
            config.directory = switchScans++ == 0 ? args[7] : originalDirectory;
            QTimer::singleShot(0, c, [&] { c->scan(config); });
            return;
        }
        if (!uploading && (ok || (config.mode == "auto" && c->hasPreview())) && operation != "preview" && operation != "switch-preview" &&
            operation != "rapid-preview" && operation != "scan-cancel") {
            uploading = true;
            if (operation == "wait") {
                std::cout << "PREVIEW_READY\n" << std::flush;
                auto *release = new QTimer(&app);
                release->setInterval(30);
                QObject::connect(release, &QTimer::timeout, &app, [&, release] {
                    if (QFile::exists(args[1] + "/go")) {
                        release->stop();
                        c->start(config);
                    }
                });
                release->start();
                return;
            }
            if (operation == "source-change") {
                QFile f(config.directory + "/source.bin");
                if (!f.open(QIODevice::Append) || f.write("changed") != 7) {
                    app.exit(65);
                    return;
                }
            }
            QTimer::singleShot(0, c, [&] { c->start(config); });
            return;
        }
        QJsonArray rows;
        for (const auto &f : c->previewModel()->records())
            rows.append(QJsonObject{{"path", f.logicalPath},
                                    {"sha256", f.sha256},
                                    {"version", f.versionId},
                                    {"status", f.status},
                                    {"change", f.change},
                                    {"note", f.note},
                                    {"size", QString::number(f.size)},
                                    {"bytes", QString::number(f.missingBytes)},
                                    {"blocks", QString::number(f.changedBlocks)},
                                    {"base", f.baseVersion}, {"allowed", f.modeAllowed},
                                    {"advice", QStringList{"unknown", "full", "incremental", "resume", "unchanged"}
                                    .at(static_cast<int>(f.advice))}, {"session", f.resumeSession},
                                    {"strategy", QStringList{"error", "full", "incremental", "resume", "skip"}
                                        .at(static_cast<int>(f.strategy))}});
        QJsonObject result{{"success", ok},
                           {"summary", summary},
                           {"rows", rows},
                           {"heartbeat", heartbeat},
                           {"history", int(window.store()->records().size())}};
        result["recommendation"] = c->recommendation();
        result["mode"] = config.mode;
        result["ready"] = c->hasPreview();
        bool visibleAdvice = false;
        for (const auto *label : window.findChildren<QLabel *>())
            if (label->isVisible() && label->text().contains(c->recommendation())) visibleAdvice = true;
        result["advice_visible"] = visibleAdvice;
        const auto screenshot = args.indexOf("--screenshot");
        if (screenshot >= 0) {
            QApplication::processEvents();
            if (screenshot+1 >= args.size() || !window.grab().save(args[screenshot+1])) { app.exit(67); return; }
        }
        std::cout << "RESULT=" << QJsonDocument(result).toJson(QJsonDocument::Compact).constData()
                  << '\n'
                  << std::flush;
        app.exit(ok ? 0 : 2);
    });
    window.show();
    QTimer::singleShot(0, c, [&] {
        auto *mode = window.findChild<QComboBox *>("backupMode");
        mode->setCurrentIndex(mode->findData(config.mode));
        c->scan(config);
        if (operation == "scan-cancel") c->cancel();
        if (operation == "rapid-preview")
            for (int n = 0; n < 5; ++n) c->scan(config);
    });
    QTimer::singleShot(90000, &app, [&] {
        c->cancel();
        app.exit(124);
    });
    return app.exec();
}
