#include "MainWindow.h"
#include "SshTunnelManager.h"
#include "TransferController.h"
#include "FileListController.h"
#include "DownloadController.h"
#include "QtTcpTransport.h"
#include "pages/SettingsPage.h"
#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QSettings>
#include <QSpinBox>
#include <QTcpServer>
#include <QTimer>
#include <iostream>
#include <algorithm>
#include <optional>
#ifdef Q_OS_WIN
#include <windows.h>
#endif
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    const auto args = app.arguments();
    if (args.size() < 6) return 64;
    const auto state = args[1], source = args[2], alias = args[3], mode = args[5];
    const auto prefix = mode == "interrupt" ? QString("managed-interrupted") : QString("managed");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QCoreApplication::setOrganizationName("ChunkHarborTests");
    QCoreApplication::setApplicationName("managed-ssh-probe");
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, state + "/settings");
    QSettings prefs;
    prefs.setValue("connection/mode", "ssh"); prefs.setValue("connection/sshHost", alias);
    prefs.setValue("connection/sshTargetPort", args[4].toUInt());
    QTcpServer reservation;
    if (!reservation.listen(QHostAddress::LocalHost)) return 65;
    prefs.setValue("connection/sshPort", reservation.serverPort()); reservation.close();
    prefs.setValue("storage/logDirectory", state + "/logs");
    prefs.setValue("task/directory", source); prefs.setValue("task/prefix", "managed");
    MainWindow window(nullptr, state + "/workspace");
    window.show();
    auto *tunnel = window.tunnelManager();
    auto *controller = window.controller();
    auto *files = window.fileListController();
    auto *download = window.downloadController();
    bool began = false, uploading = false, killed = false, completing = false, success = false;
    bool cancelledTask = false, checkingNoReplay = false;
    int pendingCheckpoints = 0;
    QString error;
    std::optional<chunkharbor::protocol::ListedFile> expectedDownload;
    qint64 lastPid = 0;
    int beats = 0;
    QTimer beat; beat.setInterval(10);
    QObject::connect(&beat, &QTimer::timeout, &app, [&] { ++beats; }); beat.start();
    const auto complete = [&](bool ok, const QString &message) {
        if (completing) return;
        completing = true; success = ok; error = message;
        window.close();
    };
    const auto confirmNoReplay = [&] {
        if (checkingNoReplay || !cancelledTask || tunnel->starts() < 2 || !tunnel->available() ||
            tunnel->state() != SshTunnelManager::State::Connected) return;
        checkingNoReplay = true;
        QTimer::singleShot(500, &app, [&] {
            QDir sessions(controller->stateDirectory());
            for (const auto &name : sessions.entryList({"*.json"}, QDir::Files)) {
                QFile saved(sessions.filePath(name));
                if (saved.open(QIODevice::ReadOnly)) {
                    const auto object = QJsonDocument::fromJson(saved.readAll()).object();
                    if (object.contains("completed") && !object["completed"].toBool()) ++pendingCheckpoints;
                }
            }
            complete(pendingCheckpoints > 0 && !controller->busy() && !controller->hasPreview() &&
                tunnel->available() && tunnel->state() == SshTunnelManager::State::Connected,
                "interrupted task stopped; checkpoint retained; reconnect did not replay task");
        });
    };
    QObject::connect(controller, &TransferController::progressChanged, &app, [&](const gui::TransferProgress &p) {
        if (mode != "interrupt" || !uploading || killed || !p.acknowledgedBytes || p.acknowledgedBytes >= p.totalBytes) return;
        killed = true;
        for (auto *child : tunnel->findChildren<QProcess *>())
            if (child->processId() == tunnel->processId()) child->kill();
    });
    QObject::connect(tunnel, &SshTunnelManager::changed, &app, [&] {
        if (completing) return;
        if (tunnel->processId()) lastPid = tunnel->processId();
        if (mode == "offline" && tunnel->state() == SshTunnelManager::State::Failed && tunnel->running()) {
            complete(!tunnel->available() && !window.findChild<QLabel *>("connectionBadge")->property("online").toBool(), tunnel->status());
            return;
        }
        if (tunnel->state() == SshTunnelManager::State::Failed && !tunnel->running()) { complete(false, tunnel->status()); return; }
        if (cancelledTask) { confirmNoReplay(); return; }
        if (tunnel->state() != SshTunnelManager::State::Connected || !tunnel->available() || began) return;
        if (mode == "reconnect" && !killed) {
            killed = true;
            QTimer::singleShot(0, tunnel, [=] {
                for (auto *child : tunnel->findChildren<QProcess *>())
                    if (child->processId() == tunnel->processId()) child->kill();
            });
            return;
        }
        began = true;
        auto config = window.findChild<SettingsPage *>()->backupConfig();
        config.directory = source; config.prefix = prefix; config.mode = "auto"; config.chunkSize = 32768;
        controller->scan(config);
    });
    QObject::connect(controller, &TransferController::finished, &app, [&](const QString &message, bool ok) {
        if (!ok) {
            if (mode == "interrupt" && uploading && killed) { cancelledTask = true; confirmNoReplay(); return; }
            complete(false, message); return;
        }
        if (!uploading) {
            uploading = true;
            auto config = window.findChild<SettingsPage *>()->backupConfig();
            config.directory = source; config.prefix = prefix; config.mode = "auto"; config.chunkSize = 32768;
            QTimer::singleShot(0, controller, [=] { controller->start(config); });
        } else if (mode == "interrupt") complete(false, "fault injection missed active upload");
        else files->refresh();
    });
    QObject::connect(files, &FileListController::finished, &app, [&](bool ok) {
        if (!ok || files->model()->page().files.empty()) { complete(false, "file list failed"); return; }
        const auto &listed = files->model()->page().files;
        const auto file = *std::max_element(listed.begin(), listed.end(), [](const auto &a, const auto &b) { return a.size < b.size; });
        expectedDownload = file;
        const auto *settings = window.findChild<SettingsPage *>();
        if (!download->start(settings->serverHost(), static_cast<quint16>(settings->port->value()),
            file, state + "/downloaded", chunkharbor::protocol::DownloadSelection::Historical)) complete(false, "download refused");
    });
    QObject::connect(download, &DownloadController::finished, &app, [&](bool ok) {
        QFile output(state + "/downloaded");
        const bool verified = ok && expectedDownload && output.open(QIODevice::ReadOnly) &&
            QCryptographicHash::hash(output.readAll(), QCryptographicHash::Sha256).toHex().toStdString() == expectedDownload->sha256;
        complete(verified, download->status());
    });
    QTimer done;
    done.setInterval(20);
    QObject::connect(&done, &QTimer::timeout, &app, [&] {
        if (!completing || tunnel->running() || window.isVisible()) return;
        bool alive = false;
#ifdef Q_OS_WIN
        if (lastPid) {
            const auto process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(lastPid));
            if (process) { alive = WaitForSingleObject(process, 0) != WAIT_OBJECT_0; CloseHandle(process); }
        }
#endif
        QJsonObject result{{"success", success && !alive}, {"detail", error}, {"starts", tunnel->starts()},
                           {"pid", QString::number(lastPid)}, {"ssh_alive_after_close", alive}, {"heartbeat", beats},
                           {"pending_checkpoints", pendingCheckpoints}};
        std::cout << "RESULT=" << QJsonDocument(result).toJson(QJsonDocument::Compact).constData() << '\n' << std::flush;
        app.exit(success && !alive ? 0 : 2);
    }); done.start();
    QTimer::singleShot(90000, &app, [&] { complete(false, "probe timeout"); });
    return app.exec();
}
