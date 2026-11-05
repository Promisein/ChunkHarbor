#include "SettingsPage.h"
#include "widgets/ComboBox.h"
#include "widgets/InfoCard.h"
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHostAddress>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStandardPaths>
#include <QVBoxLayout>
SettingsPage::SettingsPage(QWidget *parent)
    : WorkspacePage(tr("设置"), tr("连接与备份偏好保存在此设备。"), parent) {
    QSettings p;
    auto *connection = new InfoCard(tr("连接设置"), this);
    auto *f = new QFormLayout;
    connectionMode_ = new ui::ComboBox(connection);
    connectionMode_->setObjectName("connectionMode");
    connectionMode_->addItem(tr("SSH 隧道（本地转发）"), "ssh");
    connectionMode_->addItem(tr("直接 TCP（局域网 / 虚拟机）"), "tcp");
    connectionMode_->setCurrentIndex(p.value("connection/mode").toString() == "tcp" ? 1 : 0);
    host = new QLineEdit(connection);
    host->setObjectName("serverHost");
    port = new QSpinBox(connection);
    port->setObjectName("serverPort");
    port->setRange(1, 65535);
    auto *credentials = new QLineEdit(connection);
    credentials->setPlaceholderText(tr("认证功能将在 M7 提供"));
    credentials->setEnabled(false);
    auto *hostLabel = new QLabel(connection);
    auto *portLabel = new QLabel(connection);
    f->addRow(tr("连接方式"), connectionMode_);
    f->addRow(hostLabel, host);
    f->addRow(portLabel, port);
    f->addRow(tr("用户名 / 令牌"), credentials);
    connection->body()->addLayout(f);
    connectButton = new QPushButton(tr("检查连接"), connection);
    connectButton->setProperty("primary", true);
    connection->body()->addWidget(connectButton, 0, Qt::AlignLeft);
    auto *connectionHint = new QLabel(connection);
    connectionHint->setWordWrap(true);
    connection->body()->addWidget(connectionHint);
    status = new QLabel(connection);
    status->setWordWrap(true);
    connection->body()->addWidget(status);
    layout_->addWidget(connection);
    const auto loadConnection = [=] {
        const bool direct = connectionMode_->currentData() == "tcp";
        const QSignalBlocker hostSignals(host), portSignals(port);
        const QSettings settings;
        host->setReadOnly(!direct);
        host->setText(direct ? settings.value("connection/tcpHost").toString() : "127.0.0.1");
        host->setPlaceholderText(direct ? tr("输入 Ubuntu IPv4 地址或主机名") : QString{});
        port->setValue(direct ? settings.value("connection/tcpPort", 8787).toInt()
            : settings.value("connection/sshPort", settings.value("connection/port", 18787)).toInt());
        hostLabel->setText(direct ? tr("服务器地址") : tr("本地转发地址"));
        portLabel->setText(direct ? tr("服务器端口") : tr("本地转发端口"));
        connectionHint->setText(direct ? tr("直接 TCP 不提供 SSH 加密或身份认证，仅用于可信网络。")
            : tr("请先建立 SSH 本地转发。隧道不可达时连接会失败。"));
    };
    loadConnection();
    status->setText(tr("尚未连接。"));
    connect(connectionMode_, &QComboBox::currentIndexChanged, this, [=] {
        QSettings().setValue("connection/mode", connectionMode_->currentData());
        loadConnection();
        emit connectionChanged();
        emit preferencesChanged();
    });
    const auto saveEndpoint = [this] {
        QSettings settings;
        if (connectionMode_->currentData() == "tcp") {
            settings.setValue("connection/tcpHost", host->text().trimmed());
            settings.setValue("connection/tcpPort", port->value());
        } else {
            settings.setValue("connection/sshPort", port->value());
        }
        emit connectionChanged();
        emit preferencesChanged();
    };
    connect(host, &QLineEdit::textChanged, this, saveEndpoint);
    connect(port, qOverload<int>(&QSpinBox::valueChanged), this, saveEndpoint);
    auto *backup = new InfoCard(tr("备份设置"), this);
    auto *b = new QFormLayout;
    chunk_ = new ui::ComboBox(backup);
    chunk_->setObjectName("defaultChunkSize");
    for (int size : {65536, 262144, 524288})
        chunk_->addItem(QString::number(size / 1024) + " KiB", size);
    int index = chunk_->findData(p.value("backup/chunk", 262144).toInt());
    chunk_->setCurrentIndex(index < 0 ? 1 : index);
    resume_ = new QCheckBox(tr("首次/增量上传时自动续传匹配会话"), backup);
    resume_->setToolTip(tr("此项仅控制手动首次/增量模式。自动备份和显式续传始终核验匹配会话；关闭此项不会删除检查点。"));
    resume_->setChecked(p.value("backup/resume", true).toBool());
    exclusions_ = new QLineEdit(p.value("backup/exclusions", ".git;build").toString(), backup);
    exclusions_->setToolTip(tr("所有备份范围均生效，用分号分隔。旧的纯名称（.git;build）仅按大小写敏感的精确目录名排除；含 / 的相对路径按映射根匹配；*.log 匹配文件名。新增规则在 Windows 忽略大小写。与任务排除规则取并集，排除优先。"));
    b->addRow(tr("默认分块大小"), chunk_);
    b->addRow(tr("并行上传数"), new QLabel(tr("1（顺序上传，当前版本固定）"), backup));
    b->addRow(tr("校验方式"), new QLabel("SHA-256", backup));
    b->addRow(tr("断点续传"), resume_);
    b->addRow(tr("全局排除规则"), exclusions_);
    backup->body()->addLayout(b);
    layout_->addWidget(backup);
    auto *appearance = new InfoCard(tr("外观与本机存储"), this);
    auto *a = new QFormLayout;
    startup_ = new ui::ComboBox(appearance);
    const QStringList ids = {"dashboard", "backup", "files", "versions", "logs", "settings"};
    const QStringList names = {tr("总览"),     tr("备份"), tr("文件"),
                               tr("版本历史"), tr("日志"), tr("设置")};
    for (int i = 0; i < ids.size(); ++i)
        startup_->addItem(names[i], ids[i]);
    startup_->setCurrentIndex(
        qMax(0, startup_->findData(p.value("appearance/startup", "dashboard"))));
    logDirectory_ = new QLineEdit(appearance);
    logDirectory_->setReadOnly(true);
    logDirectory_->setObjectName("logDirectory");
    a->addRow(tr("主题"), new QLabel(tr("浅色（当前版本固定）"), appearance));
    a->addRow(tr("启动页面"), startup_);
    a->addRow(tr("日志目录"), logDirectory_);
    appearance->body()->addLayout(a);
    layout_->addWidget(appearance);
    layout_->addStretch();
    auto save = [this] {
        QSettings v;
        v.setValue("backup/chunk", chunk_->currentData());
        v.setValue("backup/resume", resume_->isChecked());
        v.setValue("backup/exclusions", exclusions_->text());
        v.setValue("appearance/startup", startup_->currentData());
        v.sync();
        emit preferencesChanged();
    };
    connect(chunk_, &QComboBox::currentIndexChanged, this, [save](int) { save(); });
    connect(startup_, &QComboBox::currentIndexChanged, this, [save](int) { save(); });
    connect(resume_, &QCheckBox::toggled, this, [save](bool) { save(); });
    connect(exclusions_, &QLineEdit::editingFinished, this, save);
}
gui::BackupConfig SettingsPage::backupConfig() const {
    gui::BackupConfig c;
    c.host = serverHost();
    c.direct = connectionMode_->currentData() == "tcp";
    c.port = static_cast<quint16>(port->value());
    c.chunkSize = chunk_->currentData().toUInt();
    c.resume = resume_->isChecked();
    c.exclusions = exclusions_->text().split(';', Qt::SkipEmptyParts);
    for (auto &s : c.exclusions)
        s = s.trimmed();
    return c;
}
QString SettingsPage::serverHost() const {
    if (connectionMode_->currentData() == "ssh") return "127.0.0.1";
    const auto value = host->text().trimmed().toLower();
    const QHostAddress address(value);
    if (address.protocol() == QAbstractSocket::IPv4Protocol) return address.toString();
    static const QRegularExpression dns(
        R"(^(?=.*[a-z])(?:[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?\.)*[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?\.?$)");
    return value.size() <= 253 && dns.match(value).hasMatch() ? value : QString{};
}
void SettingsPage::setLocalPort(quint16 value) {
    connectionMode_->setCurrentIndex(0);
    port->setValue(value);
}
QString SettingsPage::startupPage() const { return startup_->currentData().toString(); }
QString SettingsPage::logDirectory() const { return logDirectory_->text().trimmed(); }
