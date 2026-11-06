#include "pages/SettingsPage.h"
#include "MainWindow.h"
#include "SshTunnelManager.h"
#include "QtTcpTransport.h"
#include "FileListController.h"
#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QThread>
#include <iostream>
#include <stdexcept>
#ifdef Q_OS_WIN
#include <windows.h>
#endif
void check(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
template<class Predicate> void waitFor(Predicate predicate, int timeout = 5000) {
    QElapsedTimer clock; clock.start();
    while (!predicate() && clock.elapsed() < timeout) { QApplication::processEvents(); QThread::msleep(1); }
    check(predicate(), "managed SSH condition timed out");
}
void settle(int ms) { QElapsedTimer clock; clock.start(); waitFor([&] { return clock.elapsed() >= ms; }, ms+500); }
quint16 freePort() { QTcpServer server; check(server.listen(QHostAddress::LocalHost), "free port"); return server.serverPort(); }
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    QTemporaryDir temp;
    QCoreApplication::setOrganizationName("ChunkHarborTests");
    QCoreApplication::setApplicationName("managed-ssh");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temp.path());
    QSettings().setValue("storage/logDirectory", temp.filePath("logs"));
    try {
        SettingsPage settings;
        auto *host = settings.findChild<QLineEdit *>("sshHost");
        check(host && host->text() == "miniob-ubuntu", "automatic SSH host configuration is missing");
        host->setText("ok");
        settings.findChild<QLineEdit *>("sshUser")->setText("testuser");
        check(settings.sshConfig().host == "ok" && settings.sshConfig().user == "testuser", "SSH settings readback");
        SettingsPage restored;
        check(restored.sshConfig() == settings.sshConfig(), "SSH settings persistence");
        SshTunnelRuntime runtime;
        runtime.executable = QCoreApplication::applicationDirPath() + "/chunkharbor-ssh-process-fixture.exe";
        runtime.retryBaseMs = 30; runtime.maxRetries = 2; runtime.startupMs = 900; runtime.stopMs = 30;
        SshTunnelConfig cfg; cfg.host = "ok"; cfg.localPort = freePort();
        QtTcpTransport transport;
        SshTunnelManager tunnel(&transport, runtime);
        QObject::connect(&tunnel, &SshTunnelManager::changed, &app, [&] {
            check(tunnel.state() != SshTunnelManager::State::Connected || tunnel.available(), "stale Connected state without an owned healthy tunnel");
        });
        int pulses = 0;
        QTimer pulse; pulse.setInterval(10);
        QObject::connect(&pulse, &QTimer::timeout, &app, [&] { ++pulses; }); pulse.start();
        using State = SshTunnelManager::State;
        {
            auto slowStop = runtime; slowStop.stopMs = 350;
            QtTcpTransport switchedHealth;
            SshTunnelManager switched(&switchedHealth, slowStop);
            auto old = cfg; old.host = "lateauth";
            switched.start(old);
            waitFor([&] { return switched.processId() != 0; });
            switched.start(cfg);
            waitFor([&] { return switched.state() == State::Connected; });
            switched.stop(); waitFor([&] { return !switched.running(); });
        }
        tunnel.start(cfg);
        check(!tunnel.available(), "process start alone is not a healthy service");
        waitFor([&] { return tunnel.state() == State::Connected; });
        check(tunnel.available() && tunnel.processId() && tunnel.starts() == 1, "owned listener and HELLO");
        for (int i = 0; i < 10; ++i) { tunnel.start(cfg); tunnel.retry(); }
        waitFor([&] { return tunnel.state() == State::Connected; });
        check(tunnel.starts() == 1, "duplicate check spawned a process");
        cfg.host = "silent";
        tunnel.start(cfg);
        waitFor([&] { return tunnel.state() == State::CheckingServer; });
        check(!tunnel.available(), "open listener mistaken for HELLO");
        waitFor([&] { return tunnel.state() == State::Failed; }, 7000);
        check(tunnel.running() && tunnel.status().contains(QString::fromUtf8("备份服务")), "healthy SSH / unavailable service distinction");
        tunnel.stop(); waitFor([&] { return !tunnel.running(); });
        QTcpServer external;
        check(external.listen(QHostAddress::LocalHost), "external listener");
        cfg.host = "ok"; cfg.localPort = external.serverPort();
        const auto starts = tunnel.starts(); tunnel.start(cfg);
        check(tunnel.state() == State::Failed && tunnel.starts() == starts && external.isListening(), "external process reused or stopped");
        tunnel.stop(); external.close();
        cfg.localPort = freePort();
        cfg.host = "never";
        tunnel.start(cfg); // The preflight reservation is now released.
        check(external.listen(QHostAddress::LocalHost, cfg.localPort), "competing listener");
        waitFor([&] { return tunnel.state() == State::Failed && !tunnel.running(); });
        check(external.isListening() && !tunnel.available(), "racing foreign listener trusted or killed");
        tunnel.stop(); external.close();
        for (const auto &mode : {"auth", "hostkey", "flood"}) {
            cfg.host = mode;
            tunnel.start(cfg);
            waitFor([&] { return tunnel.state() == State::Failed && !tunnel.running(); });
            const auto count = tunnel.starts(); settle(150);
            check(tunnel.starts() == count && !tunnel.available(), "fatal SSH error retried");
        }
        cfg.host = "crash";
        const auto beforeCrash = tunnel.starts(); tunnel.start(cfg);
        waitFor([&] { return tunnel.state() == State::Failed && !tunnel.running(); }, 7000);
        check(tunnel.starts() == beforeCrash+3, "crash retry budget");
        cfg.host = "never";
        const auto beforeTimeout = tunnel.starts(); tunnel.start(cfg);
        waitFor([&] { return tunnel.state() == State::Failed && !tunnel.running(); }, 8000);
        check(tunnel.starts() == beforeTimeout+3 && pulses > 100, "startup timeout/backoff blocked GUI");
        cfg.host = "crash"; tunnel.start(cfg);
        waitFor([&] { return tunnel.state() == State::Reconnecting; });
        tunnel.stop(); const auto stoppedStarts = tunnel.starts(); settle(200);
        check(tunnel.starts() == stoppedStarts && !tunnel.running(), "disconnect did not stop retries");
        for (int i = 0; i < 20; ++i) { cfg.host = i%2 ? "ok" : "silent"; tunnel.start(cfg); }
        waitFor([&] { return tunnel.state() == State::Connected; });
        tunnel.stop(); waitFor([&] { return !tunnel.running(); });
        SshTunnelRuntime missing = runtime; missing.executable = temp.filePath("missing-ssh.exe");
        SshTunnelManager missingTunnel(&transport, missing);
        missingTunnel.start(cfg);
        check(missingTunnel.state() == State::Failed && !missingTunnel.running() &&
            missingTunnel.status().contains("ssh.exe"), "missing ssh.exe not diagnosed");
        cfg.host = "-malicious"; tunnel.start(cfg);
        check(!tunnel.running() && tunnel.state() == State::Failed, "unsafe arguments accepted");
        tunnel.stop();
        QSettings().setValue("connection/mode", "ssh");
        QSettings().setValue("connection/sshHost", "ok");
        QSettings().setValue("connection/sshPort", freePort());
        MainWindow window(nullptr, temp.filePath("workspace"), runtime);
        window.show();
        waitFor([&] { return window.tunnelManager()->state() == State::Connected; });
        auto *badge = window.findChild<QLabel *>("connectionBadge");
        check(badge && badge->property("online").toBool(), "startup auto SSH did not update GUI");
        auto *connectionMode = window.findChild<QComboBox *>("connectionMode");
        for (int i = 0; i < 10; ++i) connectionMode->setCurrentIndex(i%2 ? 0 : 1);
        waitFor([&] { return window.tunnelManager()->state() == State::Connected; });
        check(badge->property("online").toBool(), "old callback overwrote switched SSH state");
        window.resize(1060, 700); settle(30);
        check(badge->geometry().right() < badge->parentWidget()->width(), "normal window status geometry");
        window.showMaximized(); settle(30);
        check(badge->geometry().right() < badge->parentWidget()->width(), "maximized status geometry");
        if (argc > 1) {
            window.findChild<QListWidget *>("navigation")->setCurrentRow(6);
            settle(50);
            check(window.grab().save(QString::fromLocal8Bit(argv[1])), "managed SSH screenshot");
        }
        const auto pid = window.tunnelManager()->processId();
#ifdef Q_OS_WIN
        const auto process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
        check(process != nullptr, "observe owned process lifetime");
#endif
        window.close();
        waitFor([&] { return !window.isVisible() && !window.tunnelManager()->running(); });
#ifdef Q_OS_WIN
        check(WaitForSingleObject(process, 0) == WAIT_OBJECT_0, "SSH leaked after GUI close");
        CloseHandle(process);
#endif
        check(!window.findChild<SettingsPage *>()->backupConfig().host.size(), "closed SSH endpoint remained usable");
        check(external.listen(QHostAddress::LocalHost), "foreign port fixture");
        QSettings().setValue("connection/sshPort", external.serverPort());
        MainWindow conflict(nullptr, temp.filePath("conflict"), runtime);
        waitFor([&] { return conflict.tunnelManager()->state() == State::Failed; });
        check(!conflict.findChild<QLabel *>("connectionBadge")->property("online").toBool() &&
            conflict.tunnelManager()->starts() == 0 && conflict.findChild<SettingsPage *>()->backupConfig().host.isEmpty(),
            "foreign healthy-looking port trusted");
        conflict.close();
        check(external.isListening(), "foreign listener killed");
        QSettings().setValue("connection/sshPort", freePort());
        MainWindow immediatelyClosed(nullptr, temp.filePath("immediate-close"), runtime);
        immediatelyClosed.close(); settle(350);
        check(immediatelyClosed.tunnelManager()->starts() == 0, "closed window started a delayed SSH process");
        MainWindow immediatelyDisconnected(nullptr, temp.filePath("immediate-disconnect"), runtime);
        immediatelyDisconnected.findChild<SettingsPage *>()->disconnectButton->click(); settle(350);
        check(immediatelyDisconnected.tunnelManager()->starts() == 0, "explicit disconnect ignored by startup callback");
        std::cout << "managed SSH ownership, HELLO, errors, retries, duplicate clicks, settings, startup and close passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
