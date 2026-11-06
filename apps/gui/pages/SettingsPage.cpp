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
    connectionMode_->addItem(tr("SSH 隧道（自动管理）"), "ssh");
    connectionMode_->addItem(tr("直接 TCP（局域网 / 虚拟机）"), "tcp");
    connectionMode_->setCurrentIndex(p.value("connection/mode").toString() == "tcp" ? 1 : 0);
    host = new QLineEdit(connection);
    host->setObjectName("serverHost");
    port = new QSpinBox(connection);
    port->setObjectName("serverPort");
    port->setRange(1, 65535);
    auto *hostLabel = new QLabel(connection);
    auto *portLabel = new QLabel(connection);
    f->addRow(tr("连接方式"), connectionMode_);
    f->addRow(hostLabel, host);
    f->addRow(portLabel, port);
    auto *sshFields = new QWidget(connection);
    auto *sshForm = new QFormLayout(sshFields);
    sshForm->setContentsMargins(0, 0, 0, 0);
    sshHost_ = new QLineEdit(p.value("connection/sshHost", "miniob-ubuntu").toString(), sshFields);
    sshHost_->setObjectName("sshHost");
    sshUser_ = new QLineEdit(p.value("connection/sshUser").toString(), sshFields);
    sshUser_->setObjectName("sshUser");
    sshUser_->setPlaceholderText(tr("留空使用 OpenSSH 配置中的 User"));
    sshPort_ = new QSpinBox(sshFields);
    sshPort_->setObjectName("sshServerPort"); sshPort_->setRange(1, 65535);
    sshPort_->setValue(p.value("connection/sshServerPort", 22).toInt());
    sshTarget_ = new QLineEdit(p.value("connection/sshTarget", "127.0.0.1").toString(), sshFields);
    sshTarget_->setObjectName("sshTarget");
    sshTargetPort_ = new QSpinBox(sshFields);
    sshTargetPort_->setObjectName("sshTargetPort"); sshTargetPort_->setRange(1, 65535);
    sshTargetPort_->setValue(p.value("connection/sshTargetPort", 8787).toInt());
    sshForm->addRow(tr("SSH 主机 / 别名"), sshHost_);
    sshForm->addRow(tr("SSH 用户"), sshUser_);
    sshForm->addRow(tr("SSH 端口"), sshPort_);
    sshForm->addRow(tr("Ubuntu 目标地址"), sshTarget_);
    sshForm->addRow(tr("备份服务端口"), sshTargetPort_);
    f->addRow(sshFields);
    connection->body()->addLayout(f);
    connectButton = new QPushButton(tr("检查连接"), connection);
    connectButton->setProperty("primary", true);
    disconnectButton = new QPushButton(tr("断开连接"), connection);
    disconnectButton->setObjectName("disconnectConnection");
    auto *connectionActions = new QHBoxLayout;
    connectionActions->addWidget(connectButton);
    connectionActions->addWidget(disconnectButton);
    connectionActions->addStretch();
    connection->body()->addLayout(connectionActions);
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
        sshFields->setVisible(!direct);
        host->setText(direct ? settings.value("connection/tcpHost").toString() : "127.0.0.1");
        host->setPlaceholderText(direct ? tr("输入 Ubuntu IPv4 地址或主机名") : QString{});
        port->setValue(direct ? settings.value("connection/tcpPort", 8787).toInt()
            : settings.value("connection/sshPort", settings.value("connection/port", 18787)).toInt());
        hostLabel->setText(direct ? tr("服务器地址") : tr("本地转发地址"));
        portLabel->setText(direct ? tr("服务器端口") : tr("本地转发端口"));
        connectionHint->setText(direct ? tr("直接 TCP 不提供 SSH 加密或身份认证，仅用于可信网络。")
            : tr("应用自动管理 Windows OpenSSH，不需要日常打开终端。使用现有密钥和 ssh-agent；首次登录与主机指纹信任需一次性配置。外部占用的转发端口不会被复用。"));
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
    const auto saveSsh = [this] {
        QSettings settings;
        settings.setValue("connection/sshHost", sshHost_->text().trimmed());
        settings.setValue("connection/sshUser", sshUser_->text().trimmed());
        settings.setValue("connection/sshServerPort", sshPort_->value());
        settings.setValue("connection/sshTarget", sshTarget_->text().trimmed());
        settings.setValue("connection/sshTargetPort", sshTargetPort_->value());
        emit connectionChanged(); emit preferencesChanged();
    };
    for (auto *edit : {sshHost_, sshUser_, sshTarget_}) connect(edit, &QLineEdit::textChanged, this, saveSsh);
    for (auto *spin : {sshPort_, sshTargetPort_}) connect(spin, qOverload<int>(&QSpinBox::valueChanged), this, saveSsh);
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
    if (!endpointAvailable_) c.host.clear();
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
    // Explicit local TCP probe/test endpoint; never adopt an external SSH listener.
    connectionMode_->setCurrentIndex(1);
    host->setText("127.0.0.1");
    port->setValue(value);
}
bool SettingsPage::isSshMode() const { return connectionMode_->currentData() == "ssh"; }
SshTunnelConfig SettingsPage::sshConfig() const {
    return {sshHost_->text().trimmed(), sshUser_->text().trimmed(), sshTarget_->text().trimmed(),
        static_cast<quint16>(sshPort_->value()), static_cast<quint16>(port->value()), static_cast<quint16>(sshTargetPort_->value())};
}
QString SettingsPage::startupPage() const { return startup_->currentData().toString(); }
QString SettingsPage::logDirectory() const { return logDirectory_->text().trimmed(); }
