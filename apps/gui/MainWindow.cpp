#include "MainWindow.h"
#include "QtTcpTransport.h"
#include "SshTunnelManager.h"
#include "TransferController.h"
#include "FileListController.h"
#include "DownloadController.h"
#include "WorkspaceStore.h"
#include "pages/BackupPage.h"
#include "widgets/BrowserUi.h"
#include "pages/DashboardPage.h"
#include "pages/FilesPage.h"
#include "pages/LogsPage.h"
#include "pages/SettingsPage.h"
#include "pages/VersionsPage.h"
#include "pages/SnapshotPage.h"
#include <QApplication>
#include <QCloseEvent>
#include <QFile>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>
namespace {
QIcon icon(int kind) {
    QPixmap p(24, 24);
    p.fill(Qt::transparent);
    QPainter q(&p);
    q.setRenderHint(QPainter::Antialiasing);
    q.setPen(QPen(QColor("#748399"), 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    if (kind == 0) {
        for (int y : {4, 13})
            for (int x : {4, 13})
                q.drawRoundedRect(x, y, 7, 7, 1.5, 1.5);
    } else if (kind == 1) {
        q.drawRoundedRect(4, 12, 16, 8, 2, 2);
        q.drawLine(12, 16, 12, 4);
        q.drawLine(8, 8, 12, 4);
        q.drawLine(16, 8, 12, 4);
    } else if (kind == 2) {
        q.drawRoundedRect(4, 6, 16, 14, 2, 2);
        q.drawLine(4, 10, 20, 10);
    } else if (kind == 3) {
        q.drawEllipse(4, 4, 16, 16);
        q.drawLine(12, 7, 12, 12);
        q.drawLine(12, 12, 16, 14);
    } else if (kind == 4) {
        q.drawRoundedRect(5, 3, 14, 18, 2, 2);
        for (int y : {8, 12, 16})
            q.drawLine(9, y, 15, y);
    } else if (kind == 6) {
        q.drawRoundedRect(4, 7, 16, 12, 2, 2);
        q.drawLine(5, 7, 9, 4);
        q.drawLine(9, 4, 14, 4);
        q.drawLine(14, 4, 17, 7);
    } else {
        q.drawEllipse(7, 7, 10, 10);
        q.drawEllipse(10, 10, 4, 4);
        for (int i = 0; i < 8; ++i) {
            q.save();
            q.translate(12, 12);
            q.rotate(i * 45);
            q.drawLine(0, -7, 0, -9);
            q.restore();
        }
    }
    return QIcon(p);
}
} // namespace
MainWindow::MainWindow(QWidget *parent, QString dataDirectory)
    : MainWindow(parent, std::move(dataDirectory), SshTunnelRuntime{}) {}
MainWindow::MainWindow(QWidget *parent, QString dataDirectory, StartupMode startup)
    : MainWindow(parent, std::move(dataDirectory), SshTunnelRuntime{}, startup) {}
MainWindow::MainWindow(QWidget *parent, QString dataDirectory, SshTunnelRuntime runtime, StartupMode startupMode)
    : QMainWindow(parent), transport_(new QtTcpTransport(this)) {
    startupMode_ = startupMode;
    tunnel_ = new SshTunnelManager(transport_, std::move(runtime), this);
    connectionTimer_ = new QTimer(this);
    connectionTimer_->setSingleShot(true);
    connectionTimer_->setInterval(250);
    store_ = new WorkspaceStore(std::move(dataDirectory), this);
    controller_ = new TransferController(store_, this);
    fileList_ = new FileListController(this);
    download_ = new DownloadController(this);
    setWindowTitle(tr("ChunkHarbor"));
    resize(1280, 820);
    setMinimumSize(1060, 700);
    QFile sheet(":/styles/Light.qss");
    if (sheet.open(QIODevice::ReadOnly))
        setStyleSheet(QString::fromUtf8(sheet.readAll()));
    auto *root = new QWidget(this);
    root->setObjectName("workspace");
    auto *horizontal = new QHBoxLayout(root);
    horizontal->setContentsMargins(0, 0, 0, 0);
    horizontal->setSpacing(0);
    auto *sidebar = new QFrame(root);
    sidebar->setObjectName("sidebar");
    sidebar->setFixedWidth(188);
    auto *side = new QVBoxLayout(sidebar);
    side->setContentsMargins(18, 30, 18, 24);
    side->setSpacing(26);
    auto *brand = new QLabel(tr("ChunkHarbor"), sidebar);
    brand->setProperty("role", "brand");
    side->addWidget(brand);
    auto *nav = new QListWidget(sidebar);
    nav->setObjectName("navigation");
    nav->setAccessibleName(tr("页面导航"));
    nav->setFocusPolicy(Qt::StrongFocus);
    nav->setSpacing(2);
    nav->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    const QStringList names = {tr("总览"),     tr("备份"), tr("文件"),
                               tr("版本历史"), tr("目录快照"), tr("日志"), tr("设置")};
    const int iconOrder[] = {0, 1, 2, 3, 6, 4, 5};
    for (int i = 0; i < names.size(); ++i) {
        auto *item = new QListWidgetItem(icon(iconOrder[i]), names[i], nav);
        item->setSizeHint(QSize(144, 46));
    }
    side->addWidget(nav, 1);
    auto *footer = new QLabel(tr("本地工作区\n单向增量备份"), sidebar);
    footer->setProperty("role", "muted");
    side->addWidget(footer);
    horizontal->addWidget(sidebar);
    auto *right = new QVBoxLayout;
    right->setSpacing(0);
    right->setContentsMargins(0, 0, 0, 0);
    horizontal->addLayout(right, 1);
    auto *bar = new QFrame(root);
    bar->setObjectName("topbar");
    auto *top = new QHBoxLayout(bar);
    top->setContentsMargins(26, 10, 22, 10);
    top->setSpacing(12);
    auto *title = new QLabel(names[0], bar);
    auto f = title->font();
    f.setBold(true);
    title->setFont(f);
    top->addWidget(title);
    top->addStretch(1);
    auto *badge = new QLabel(bar);
    badge->setObjectName("connectionBadge");
    badge->setToolTip(tr("点击检测连接验证服务器可用性。"));
    top->addWidget(badge);
    auto *address = new ui::ElidedLabel(bar);
    address->setObjectName("connectionAddress");
    address->setProperty("role", "muted");
    address->setAlignment(Qt::AlignCenter);
    top->addWidget(address, 1);
    auto *refresh = new QPushButton(bar);
    refresh->setObjectName("detectConnection");
    refresh->setProperty("primary", true);
    top->addWidget(refresh);
    auto *preferences = new QPushButton(bar);
    preferences->setObjectName("topbarSettings");
    preferences->setIcon(icon(5));
    preferences->setToolTip(tr("设置"));
    preferences->setAccessibleName(tr("打开设置"));
    top->addWidget(preferences);
    right->addWidget(bar);
    auto *stack = new QStackedWidget(root);
    stack->setObjectName("pageStack");
    settings_ = new SettingsPage;
    fileList_->setEndpoint(settings_->serverHost(), static_cast<quint16>(settings_->port->value()));
    auto updateAddress = [this, address] {
        auto host = settings_->host->text().trimmed();
        if (host.isEmpty()) host = tr("未配置地址");
        if (host.contains(':') && !host.startsWith('[')) host = "[" + host + "]";
        const auto endpoint = QString("%1:%2").arg(host).arg(settings_->port->value());
        address->setMaximumWidth(qMin(236, address->fontMetrics().horizontalAdvance(endpoint) + 16));
        address->setValue(endpoint);
    };
    updateAddress();
    settings_->findChild<QLineEdit *>("logDirectory")->setText(store_->logDirectory());
    auto *filesPage = new FilesPage(fileList_, nullptr, download_);
    auto *versionsPage = new VersionsPage(store_, nullptr, download_);
    versionsPage->setServerEndpoint(settings_->serverHost(), static_cast<quint16>(settings_->port->value()));
    auto *snapshotPage = snapshots_ = new SnapshotPage;
    snapshotPage->setEndpoint(settings_->serverHost(), static_cast<quint16>(settings_->port->value()));
    const QList<QWidget *> pages = {new DashboardPage(controller_, store_, transport_),
                                    new BackupPage(controller_, settings_),
                                    filesPage,
                                    versionsPage,
                                    snapshotPage,
                                    new LogsPage(store_),
                                    settings_};
    int settingsIndex = -1;
    for (auto *page : pages) {
        if (page == filesPage || page == versionsPage || page == snapshotPage) {
            stack->addWidget(page);
            continue;
        }
        auto *scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidget(page);
        const int index = stack->addWidget(scroll);
        if (page == settings_) settingsIndex = index;
    }
    right->addWidget(stack, 1);
    // Reparent only the presentation bar; the window-owned download survives navigation.
    right->addWidget(filesPage->transferBar());
    connect(nav, &QListWidget::currentRowChanged, this, [stack, title, names](int row) {
        if (row >= 0 && row < names.size()) {
            stack->setCurrentIndex(row);
            title->setText(names[row]);
        }
    });
    const QStringList pageIds = {"dashboard", "backup", "files", "versions", "snapshots", "logs", "settings"};
    nav->setCurrentRow(qMax(0, pageIds.indexOf(settings_->startupPage())));
    connect(preferences, &QPushButton::clicked, this, [nav, settingsIndex] { nav->setCurrentRow(settingsIndex); });
    using Health = QtTcpTransport::HealthState;
    auto check = [this] {
        if (closing_ || !settings_->isEnabled() || transport_->healthState() == Health::Checking) return;
        userDisconnected_ = false;
        connectionTimer_->stop();
        if (settings_->isSshMode()) {
            tunnel_->start(settings_->sshConfig());
            tunnel_->retry();
            return;
        }
        if (tunnel_->running()) { directProbePending_ = true; tunnel_->stop(); return; }
        transport_->connectToServer(settings_->serverHost(),
                                    static_cast<quint16>(settings_->port->value()));
    };
    connect(refresh, &QPushButton::clicked, this, check);
    connect(settings_->connectButton, &QPushButton::clicked, this, check);
    auto syncBusinessEndpoint = [this, versionsPage, snapshotPage] {
        const bool available = settings_->isSshMode() ? tunnel_->available() : !tunnel_->running();
        settings_->setEndpointAvailable(available);
        const auto host = available ? settings_->serverHost() : QString{};
        const auto port = available ? static_cast<quint16>(settings_->port->value()) : quint16(0);
        fileList_->setEndpoint(host, port);
        versionsPage->setServerEndpoint(host, port);
        snapshotPage->setEndpoint(host, port);
    };
    auto endpointChanged = [this, updateAddress, syncBusinessEndpoint] {
        updateAddress();
        transport_->disconnectFromServer();
        connectionTimer_->stop(); directProbePending_ = false;
        userDisconnected_ = false;
        tunnel_->stop();
        syncBusinessEndpoint();
        if (!closing_ && startupMode_ == StartupMode::Automatic && settings_->isSshMode()) connectionTimer_->start();
    };
    connect(settings_, &SettingsPage::connectionChanged, this, endpointChanged);
    connect(connectionTimer_, &QTimer::timeout, this, [this] {
        if (!closing_ && !userDisconnected_ && settings_->isSshMode()) tunnel_->start(settings_->sshConfig());
    });
    connect(settings_->disconnectButton, &QPushButton::clicked, this, [this] {
        connectionTimer_->stop(); directProbePending_ = false;
        userDisconnected_ = true;
        tunnel_->stop(); transport_->disconnectFromServer();
    });
    connect(transport_, &QtTcpTransport::statusChanged, this, [this, badge](const QString &text) {
        if (settings_->isSshMode()) return;
        store_->appendLog("信息", text);
        settings_->status->setText(text);
        badge->setToolTip(ui::plainTooltip(text));
    });
    auto updateHealth = [this, badge, refresh] {
        const auto state = transport_->healthState();
        badge->setText(transport_->healthStateText());
        badge->setProperty("online", state == Health::Reachable);
        switch (state) {
        case Health::Unchecked:
            badge->setProperty("health", "unchecked");
            refresh->setText(tr("检测连接"));
            break;
        case Health::Checking:
            badge->setProperty("health", "checking");
            refresh->setText(tr("检测中"));
            break;
        case Health::Reachable:
            badge->setProperty("health", "reachable");
            refresh->setText(tr("重新检测"));
            break;
        case Health::Unreachable:
            badge->setProperty("health", "unreachable");
            refresh->setText(tr("重试检测"));
            break;
        }
        refresh->setEnabled(state != Health::Checking && !controller_->busy());
        if (settings_->isSshMode()) {
            const auto ssh = tunnel_->state();
            if (ssh != SshTunnelManager::State::Connected) {
                QString label;
                switch (ssh) {
                case SshTunnelManager::State::Disabled: label = tr("未检测"); break;
                case SshTunnelManager::State::Starting: label = tr("正在建立 SSH"); break;
                case SshTunnelManager::State::TunnelReady: label = tr("隧道已就绪"); break;
                case SshTunnelManager::State::CheckingServer: label = tr("检测中"); break;
                case SshTunnelManager::State::Reconnecting: label = tr("SSH 重连中"); break;
                case SshTunnelManager::State::Stopping: label = tr("正在关闭 SSH"); break;
                default: label = tr("连接失败"); break;
                }
                badge->setText(label);
                badge->setProperty("online", false);
                badge->setProperty("health", ssh == SshTunnelManager::State::Failed ? "unreachable" : "checking");
            }
            refresh->setEnabled(settings_->isEnabled() && ssh != SshTunnelManager::State::Starting &&
                ssh != SshTunnelManager::State::Stopping && ssh != SshTunnelManager::State::CheckingServer);
        }
        badge->style()->unpolish(badge);
        badge->style()->polish(badge);
    };
    connect(transport_, &QtTcpTransport::healthChanged, this, updateHealth);
    connect(tunnel_, &SshTunnelManager::changed, this, [this, badge, syncBusinessEndpoint, updateHealth] {
        syncBusinessEndpoint(); updateHealth();
        if (!settings_->isSshMode()) return;
        settings_->status->setText(tunnel_->status());
        badge->setToolTip(ui::plainTooltip(tunnel_->status()));
        if (!tunnel_->status().isEmpty()) store_->appendLog("信息", tunnel_->status());
    });
    connect(tunnel_, &SshTunnelManager::tunnelLost, this, [this] {
        controller_->cancel(); download_->cancel();
        snapshots_->cancelRestore(); snapshots_->cancelCaptureBeforeSend();
        controller_->invalidatePreview();
    });
    connect(tunnel_, &SshTunnelManager::stopped, this, [this] {
        if (closing_) QTimer::singleShot(0, this, [this] { if (closing_) close(); });
        else if (directProbePending_ && !settings_->isSshMode()) {
            directProbePending_ = false;
            transport_->connectToServer(settings_->serverHost(), static_cast<quint16>(settings_->port->value()));
        }
    });
    const auto updateConnectionLock = [this, snapshotPage] {
        settings_->setEnabled(!controller_->busy() && !download_->busy() &&
            !snapshotPage->restoring() && !snapshotPage->settlingCapture());
    };
    connect(controller_, &TransferController::busyChanged, this, updateHealth);
    connect(controller_, &TransferController::busyChanged, settings_, updateConnectionLock);
    connect(download_, &DownloadController::changed, settings_, updateConnectionLock);
    connect(snapshotPage, &SnapshotPage::connectionLockChanged, settings_, updateConnectionLock);
    updateHealth();
    connect(controller_, &TransferController::finished, this, [this](const QString &, bool) {
        if (closing_)
            QTimer::singleShot(0, this, [this] { if (closing_) close(); });
    });
    connect(download_, &DownloadController::finished, this, [this](bool) {
        if (closing_) QTimer::singleShot(0, this, [this] { if (closing_) close(); });
    });
    connect(snapshots_, &SnapshotPage::restoreFinished, this, [this, nav, stack](const QString &residue) {
        if (!residue.isEmpty()) {
            const auto message = tr("快照恢复清理失败，暂存目录仍在 %1").arg(residue);
            store_->appendLog("错误", message);
            statusBar()->showMessage(message);
            if (closing_) {
                closing_ = false;
                setWindowTitle(tr("ChunkHarbor"));
                nav->setCurrentRow(stack->indexOf(snapshots_));
            }
            return;
        }
        if (closing_) QTimer::singleShot(0, this, [this] { if (closing_) close(); });
    });
    connect(snapshots_, &SnapshotPage::captureSettled, this, [this, nav, stack](bool uncertain) {
        if (!closing_) return;
        if (uncertain) {
            closing_ = false;
            setWindowTitle(tr("ChunkHarbor"));
            nav->setCurrentRow(stack->indexOf(snapshots_));
        } else {
            QTimer::singleShot(0, this, [this] { if (closing_) close(); });
        }
    });
    connect(store_, &WorkspaceStore::storageError, this, [this](const QString &message) {
        settings_->status->setText(message);
        statusBar()->showMessage(message, 15000);
    });
    setCentralWidget(root);
    syncBusinessEndpoint();
    if (startupMode == StartupMode::Automatic) QTimer::singleShot(0, this, [this, check] {
        if (closing_ || userDisconnected_) return;
        if (settings_->isSshMode()) tunnel_->start(settings_->sshConfig());
        else if (transport_->healthState() == Health::Unchecked) check();
    });
}
MainWindow::~MainWindow() {
    connectionTimer_->stop();
    tunnel_->disconnect(this);
    transport_->disconnect(this);
    fileList_->cancel();
    // Quiesce the probe while its timers and UI observers still exist. Otherwise
    // socket destruction can emit disconnected after transport members are gone.
    transport_->disconnectFromServer();
    // Views capture their controllers. Disconnect/destroy views before models or
    // controllers can emit teardown signals; then join workers while QObject lives.
    delete takeCentralWidget();
    delete download_;
    delete controller_;
    delete tunnel_;
}
void MainWindow::closeEvent(QCloseEvent *event) {
    closing_ = true;
    connectionTimer_->stop(); directProbePending_ = false; tunnel_->stopRetries();
    snapshots_->cancelCaptureBeforeSend();
    if (controller_->busy() || download_->busy() || snapshots_->restoring() || snapshots_->settlingCapture()) {
        closing_ = true;
        controller_->cancel();
        download_->cancel();
        snapshots_->cancelRestore();
        event->ignore();
        setWindowTitle(tr("ChunkHarbor · 正在保存进度并退出…"));
        return;
    }
    if (tunnel_->running()) {
        closing_ = true;
        tunnel_->stop(); event->ignore();
        setWindowTitle(tr("ChunkHarbor · 正在关闭 SSH 隧道…"));
        return;
    }
    transport_->disconnectFromServer();
    event->accept();
}
void MainWindow::connectToLocalServer(quint16 port) {
    settings_->setLocalPort(port);
    transport_->connectToServer("127.0.0.1", port);
}
