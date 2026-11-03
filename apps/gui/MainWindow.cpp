#include "MainWindow.h"
#include "QtTcpTransport.h"
#include "TransferController.h"
#include "FileListController.h"
#include "WorkspaceStore.h"
#include "pages/BackupPage.h"
#include "pages/DashboardPage.h"
#include "pages/FilesPage.h"
#include "pages/LogsPage.h"
#include "pages/SettingsPage.h"
#include "pages/VersionsPage.h"
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
    : QMainWindow(parent), transport_(new QtTcpTransport(this)) {
    store_ = new WorkspaceStore(std::move(dataDirectory), this);
    controller_ = new TransferController(store_, this);
    fileList_ = new FileListController(this);
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
    sidebar->setFixedWidth(216);
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
    const QStringList names = {tr("总览"),     tr("备份"), tr("文件"),
                               tr("版本历史"), tr("日志"), tr("设置")};
    for (int i = 0; i < names.size(); ++i) {
        auto *item = new QListWidgetItem(icon(i), names[i], nav);
        item->setSizeHint(QSize(170, 46));
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
    top->setContentsMargins(32, 20, 28, 20);
    top->setSpacing(16);
    auto *title = new QLabel(names[0], bar);
    auto f = title->font();
    f.setBold(true);
    title->setFont(f);
    top->addWidget(title);
    top->addStretch();
    auto *badge = new QLabel(tr("未检查"), bar);
    badge->setObjectName("connectionBadge");
    badge->setToolTip(tr("尚未检查；点击刷新后每 15 秒使用独立 HELLO 检查服务器。"));
    top->addWidget(badge);
    auto *address = new QLabel(bar);
    address->setProperty("role", "muted");
    top->addWidget(address);
    auto *refresh = new QPushButton(bar);
    refresh->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
    refresh->setToolTip(tr("检查服务器连接"));
    refresh->setAccessibleName(tr("刷新连接"));
    top->addWidget(refresh);
    auto *preferences = new QPushButton(bar);
    preferences->setIcon(icon(5));
    preferences->setToolTip(tr("设置"));
    preferences->setAccessibleName(tr("打开设置"));
    top->addWidget(preferences);
    right->addWidget(bar);
    auto *stack = new QStackedWidget(root);
    stack->setObjectName("pageStack");
    settings_ = new SettingsPage;
    fileList_->setEndpoint(settings_->host->text(), static_cast<quint16>(settings_->port->value()));
    address->setText(QString("127.0.0.1:%1").arg(settings_->port->value()));
    auto updatePreferences = [this] {
        auto dir = settings_->logDirectory();
        if (!dir.isEmpty() && dir != store_->logDirectory())
            store_->setLogDirectory(dir);
    };
    connect(settings_, &SettingsPage::preferencesChanged, this, updatePreferences);
    updatePreferences();
    const QList<QWidget *> pages = {new DashboardPage(controller_, store_, transport_),
                                    new BackupPage(controller_, settings_),
                                    new FilesPage(fileList_),
                                    new VersionsPage(store_),
                                    new LogsPage(store_),
                                    settings_};
    for (auto *page : pages) {
        auto *scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidget(page);
        stack->addWidget(scroll);
    }
    right->addWidget(stack, 1);
    connect(nav, &QListWidget::currentRowChanged, this, [stack, title, names](int row) {
        if (row >= 0 && row < names.size()) {
            stack->setCurrentIndex(row);
            title->setText(names[row]);
        }
    });
    const QStringList pageIds = {"dashboard", "backup", "files", "versions", "logs", "settings"};
    nav->setCurrentRow(qMax(0, pageIds.indexOf(settings_->startupPage())));
    connect(preferences, &QPushButton::clicked, this, [nav] { nav->setCurrentRow(5); });
    auto check = [this] {
        transport_->connectToServer(settings_->host->text(),
                                    static_cast<quint16>(settings_->port->value()));
    };
    connect(refresh, &QPushButton::clicked, this, check);
    connect(settings_->connectButton, &QPushButton::clicked, this, check);
    connect(settings_->port, qOverload<int>(&QSpinBox::valueChanged), this,
            [this, address](int port) {
                address->setText(QString("127.0.0.1:%1").arg(port));
                transport_->disconnectFromServer();
                fileList_->setEndpoint(settings_->host->text(), static_cast<quint16>(port));
            });
    connect(transport_, &QtTcpTransport::statusChanged, this, [this, badge](const QString &text) {
        store_->appendLog("信息", text);
        settings_->status->setText(text);
        badge->setToolTip(text);
    });
    connect(transport_, &QtTcpTransport::healthChanged, this, [this, badge] {
        badge->setText(transport_->healthStateText());
        badge->setProperty("online", transport_->healthState() == QtTcpTransport::HealthState::Reachable);
        badge->style()->unpolish(badge);
        badge->style()->polish(badge);
    });
    connect(controller_, &TransferController::busyChanged, this, [this, refresh](bool busy) {
        settings_->setEnabled(!busy);
        refresh->setEnabled(!busy);
    });
    connect(controller_, &TransferController::finished, this, [this](const QString &, bool) {
        if (closing_)
            QTimer::singleShot(0, this, [this] { close(); });
    });
    connect(store_, &WorkspaceStore::storageError, this, [this](const QString &message) {
        settings_->status->setText(message);
        statusBar()->showMessage(message, 15000);
    });
    setCentralWidget(root);
}
MainWindow::~MainWindow() { delete controller_; }
void MainWindow::closeEvent(QCloseEvent *event) {
    if (controller_->busy()) {
        closing_ = true;
        controller_->cancel();
        event->ignore();
        setWindowTitle(tr("ChunkHarbor · 正在保存进度并退出…"));
        return;
    }
    event->accept();
}
void MainWindow::connectToLocalServer(quint16 port) {
    settings_->port->setValue(port);
    transport_->connectToServer("127.0.0.1", port);
}
