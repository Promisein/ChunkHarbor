#include "MainWindow.h"
#include "TransferController.h"
#include "WorkspaceStore.h"
#include <QApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTimer>
#include <iostream>
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    if (argc < 7)
        return 64;
    const auto args = app.arguments();
    QCoreApplication::setOrganizationName("ChunkHarborTests");
    QCoreApplication::setApplicationName("gui-transfer-probe");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, args[1] + "/settings");
    QSettings().setValue("storage/logDirectory", args[1] + "/logs");
    MainWindow window(nullptr, args[1]);
    auto *c = window.controller();
    gui::BackupConfig config;
    config.directory = args[2];
    config.port = args[3].toUShort();
    config.prefix = args[4];
    config.chunkSize = 32768;
    config.mode = args[6];
    QString operation = args[5];
    const auto originalDirectory = config.directory;
    int switchScans = 0;
    bool uploading = false, interrupted = false;
    int heartbeat = 0;
    QTimer beat;
    beat.setInterval(10);
    QObject::connect(&beat, &QTimer::timeout, &app, [&] { ++heartbeat; });
    beat.start();
    QObject::connect(c, &TransferController::progressChanged, &app,
                     [&](const gui::TransferProgress &p) {
                         if (uploading && p.acknowledgedBytes) {
                             std::cout << "ACK=" << p.acknowledgedBytes << '\n' << std::flush;
                             if ((operation == "cancel" || operation == "cancel-large") && !interrupted &&
                                 (operation != "cancel-large" || p.acknowledgedBytes >= 32768)) {
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
        if (operation == "switch-preview" && ok && switchScans < 2 && args.size() > 7) {
            config.directory = switchScans++ == 0 ? args[7] : originalDirectory;
            QTimer::singleShot(0, c, [&] { c->scan(config); });
            return;
        }
        if (!uploading && ok && operation != "preview" && operation != "switch-preview" &&
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
                                    {"blocks", QString::number(f.changedBlocks)}});
        QJsonObject result{{"success", ok},
                           {"summary", summary},
                           {"rows", rows},
                           {"heartbeat", heartbeat},
                           {"history", int(window.store()->records().size())}};
        std::cout << "RESULT=" << QJsonDocument(result).toJson(QJsonDocument::Compact).constData()
                  << '\n'
                  << std::flush;
        app.exit(ok ? 0 : 2);
    });
    window.show();
    QTimer::singleShot(0, c, [&] {
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
