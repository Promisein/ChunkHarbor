#include "SettingsPage.h"
#include "widgets/ComboBox.h"
#include "widgets/InfoCard.h"
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QVBoxLayout>
SettingsPage::SettingsPage(QWidget *parent)
    : WorkspacePage(tr("设置"), tr("连接与备份偏好保存在此设备。"), parent) {
    QSettings p;
    auto *connection = new InfoCard(tr("连接设置"), this);
    auto *f = new QFormLayout;
    host = new QLineEdit("127.0.0.1", connection);
    host->setReadOnly(true);
    host->setToolTip(tr("通过本地地址或 SSH 隧道连接 Ubuntu。"));
    port = new QSpinBox(connection);
    port->setObjectName("serverPort");
    port->setRange(1, 65535);
    port->setValue(p.value("connection/port", 18787).toInt());
    auto *credentials = new QLineEdit(connection);
    credentials->setPlaceholderText(tr("认证功能将在 M7 提供"));
    credentials->setEnabled(false);
    f->addRow(tr("服务器地址"), host);
    f->addRow(tr("端口"), port);
    f->addRow(tr("用户名 / 令牌"), credentials);
    connection->body()->addLayout(f);
    connectButton = new QPushButton(tr("检查连接"), connection);
    connectButton->setProperty("primary", true);
    connection->body()->addWidget(connectButton, 0, Qt::AlignLeft);
    status = new QLabel(tr("尚未连接，请先启动服务端与 SSH 隧道。"), connection);
    status->setWordWrap(true);
    connection->body()->addWidget(status);
    layout_->addWidget(connection);
    auto *backup = new InfoCard(tr("备份设置"), this);
    auto *b = new QFormLayout;
    chunk_ = new ui::ComboBox(backup);
    chunk_->setObjectName("defaultChunkSize");
    for (int size : {65536, 262144, 524288})
        chunk_->addItem(QString::number(size / 1024) + " KiB", size);
    int index = chunk_->findData(p.value("backup/chunk", 262144).toInt());
    chunk_->setCurrentIndex(index < 0 ? 1 : index);
    resume_ = new QCheckBox(tr("保留会话并恢复已确认进度"), backup);
    resume_->setChecked(p.value("backup/resume", true).toBool());
    exclusions_ = new QLineEdit(p.value("backup/exclusions", ".git;build").toString(), backup);
    exclusions_->setToolTip(tr("目录名或相对路径，用分号分隔。"));
    b->addRow(tr("默认分块大小"), chunk_);
    b->addRow(tr("并行上传数"), new QLabel(tr("1（顺序上传，当前版本固定）"), backup));
    b->addRow(tr("校验方式"), new QLabel("SHA-256", backup));
    b->addRow(tr("断点续传"), resume_);
    b->addRow(tr("排除规则"), exclusions_);
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
    logDirectory_ = new QLineEdit(
        p.value("storage/logDirectory",
                QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/logs")
            .toString(),
        appearance);
    logDirectory_->setObjectName("logDirectory");
    a->addRow(tr("主题"), new QLabel(tr("浅色（当前版本固定）"), appearance));
    a->addRow(tr("启动页面"), startup_);
    a->addRow(tr("日志目录"), logDirectory_);
    appearance->body()->addLayout(a);
    layout_->addWidget(appearance);
    layout_->addStretch();
    auto save = [this] {
        QSettings v;
        v.setValue("connection/port", port->value());
        v.setValue("backup/chunk", chunk_->currentData());
        v.setValue("backup/resume", resume_->isChecked());
        v.setValue("backup/exclusions", exclusions_->text());
        v.setValue("appearance/startup", startup_->currentData());
        v.setValue("storage/logDirectory", logDirectory_->text());
        v.sync();
        emit preferencesChanged();
    };
    connect(port, qOverload<int>(&QSpinBox::valueChanged), this, [save](int) { save(); });
    connect(chunk_, &QComboBox::currentIndexChanged, this, [save](int) { save(); });
    connect(startup_, &QComboBox::currentIndexChanged, this, [save](int) { save(); });
    connect(resume_, &QCheckBox::toggled, this, [save](bool) { save(); });
    connect(exclusions_, &QLineEdit::editingFinished, this, save);
    connect(logDirectory_, &QLineEdit::editingFinished, this, save);
}
gui::BackupConfig SettingsPage::backupConfig() const {
    gui::BackupConfig c;
    c.port = static_cast<quint16>(port->value());
    c.chunkSize = chunk_->currentData().toUInt();
    c.resume = resume_->isChecked();
    c.exclusions = exclusions_->text().split(';', Qt::SkipEmptyParts);
    for (auto &s : c.exclusions)
        s = s.trimmed();
    return c;
}
QString SettingsPage::startupPage() const { return startup_->currentData().toString(); }
QString SettingsPage::logDirectory() const { return logDirectory_->text().trimmed(); }
