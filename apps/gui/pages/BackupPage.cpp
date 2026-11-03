#include "BackupPage.h"
#include "SettingsPage.h"
#include "TransferController.h"
#include "widgets/ComboBox.h"
#include "widgets/InfoCard.h"
#include <QComboBox>
#include <QFileDialog>
#include <QGridLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QTableView>
#include <QVBoxLayout>
BackupPage::BackupPage(TransferController *controller, SettingsPage *settings, QWidget *parent)
    : WorkspacePage(tr("备份"), tr("扫描真实目录，预览变化，再提交到 Ubuntu 服务端。"), parent) {
    layout_->setSpacing(12);
    layout_->setContentsMargins(28, 18, 28, 20);
    QSettings prefs;
    auto *config = new InfoCard(tr("备份配置"), this);
    auto *grid = new QGridLayout;
    grid->setHorizontalSpacing(12);
    grid->setVerticalSpacing(10);
    auto *local = new QLineEdit(prefs.value("task/directory").toString(), config);
    local->setObjectName("backupDirectory");
    local->setPlaceholderText(tr("选择需要备份的本地目录"));
    auto *remote = new QLineEdit(prefs.value("task/prefix", "workspace").toString(), config);
    remote->setObjectName("backupPrefix");
    remote->setToolTip(tr("服务端根目录内的相对逻辑路径，例如 workspace/documents。"));
    auto *browse = new QPushButton(tr("浏览…"), config);
    auto *mode = new ui::ComboBox(config);
    mode->setObjectName("backupMode");
    mode->addItem(tr("自动备份（推荐）"), "auto");
    mode->addItem(tr("首次完整上传"), "full");
    mode->addItem(tr("断点续传"), "resume");
    mode->addItem(tr("增量备份"), "incremental");
    mode->setCurrentIndex(qMax(0, mode->findData(prefs.value("task/mode", "auto"))));
    mode->setToolTip(tr("自动备份按文件选择首次、增量、续传或跳过；手动模式保留严格校验。"));
    auto *scan = new QPushButton(tr("扫描并预览"), config);
    scan->setObjectName("backupScan");
    auto *start = new QPushButton(tr("开始备份"), config);
    start->setObjectName("backupStart");
    start->setProperty("primary", true);
    grid->addWidget(new QLabel(tr("本地目录")), 0, 0);
    grid->addWidget(local, 0, 1);
    grid->addWidget(browse, 0, 2);
    grid->addWidget(mode, 0, 3);
    grid->addWidget(new QLabel(tr("远端路径")), 1, 0);
    grid->addWidget(remote, 1, 1);
    grid->addWidget(scan, 1, 2);
    grid->addWidget(start, 1, 3);
    grid->setColumnStretch(1, 1);
    config->body()->addLayout(grid);
    layout_->addWidget(config);
    auto *preview = new InfoCard(tr("变更预览"), this);
    preview->setSubtitle(tr("尚未扫描。每个文件上限 64 MiB，符号链接不会被备份。"));
    auto *table = new QTableView(preview);
    table->setObjectName("backupTable");
    table->setModel(controller->previewModel());
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setShowGrid(false);
    table->setAlternatingRowColors(true);
    table->verticalHeader()->hide();
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    // Keep the four-character action labels readable with the existing QSS padding.
    table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Fixed);
    table->setColumnWidth(4, 112);
    table->horizontalHeader()->setStretchLastSection(true);
    table->verticalHeader()->setDefaultSectionSize(28);
    table->horizontalHeader()->setFixedHeight(30);
    table->setMinimumHeight(146);
    preview->body()->addWidget(table, 1);
    layout_->addWidget(preview, 1);
    auto *progress = new InfoCard(tr("任务进度"), this);
    auto *current = new QLabel(tr("尚未开始"), progress);
    current->setObjectName("currentFile");
    auto *phase = new QLabel(tr("请先扫描目录并获取服务器差异计划"), progress);
    phase->setObjectName("taskPhase");
    phase->setWordWrap(true);
    auto *bar = new QProgressBar(progress);
    bar->setTextVisible(false);
    auto *percent = new QLabel("0%", progress);
    percent->setObjectName("taskPercent");
    percent->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    bar->setRange(0, 100);
    bar->setValue(0);
    bar->setObjectName("taskProgress");
    auto *metrics = new QLabel(tr("持久确认：0 B / 0 B　速度：—　预计剩余：—"), progress);
    metrics->setWordWrap(true);
    auto *actions = new QHBoxLayout;
    auto *pause = new QPushButton(tr("暂停"), progress);
    pause->setObjectName("backupPause");
    auto *cancel = new QPushButton(tr("取消"), progress);
    cancel->setObjectName("backupCancel");
    auto *retry = new QPushButton(tr("重试 / 恢复"), progress);
    retry->setObjectName("backupRetry");
    retry->setToolTip(tr("按上次配置重新扫描并核验会话；确认新预览后点击开始备份。"));
    pause->setToolTip(tr("在安全请求边界暂停，当前请求可能继续完成。"));
    cancel->setToolTip(tr("停止任务并保留已确认进度，供后续恢复。"));
    actions->addWidget(phase, 1);
    actions->addWidget(pause);
    actions->addWidget(cancel);
    actions->addWidget(retry);
    auto *progressHeading = new QHBoxLayout;
    progressHeading->addWidget(current, 1);
    progressHeading->addWidget(percent);
    progress->body()->addLayout(progressHeading);
    progress->body()->addWidget(bar);
    progress->body()->addWidget(metrics);
    progress->body()->addLayout(actions);
    layout_->addWidget(progress);
    for (auto *card : {config, preview, progress}) {
        card->body()->setContentsMargins(18, 12, 18, 14);
        card->body()->setSpacing(8);
    }
    auto configuration = [=] {
        auto c = settings->backupConfig();
        c.directory = local->text().trimmed();
        c.prefix = remote->text().trimmed();
        c.mode = mode->currentData().toString();
        QSettings p;
        p.setValue("task/directory", c.directory);
        p.setValue("task/prefix", c.prefix);
        p.setValue("task/mode", c.mode);
        return c;
    };
    auto enable = [=] {
        bool busy = controller->busy();
        for (auto *w : QList<QWidget *>{local, remote, browse, mode, scan})
            w->setEnabled(!busy);
        start->setEnabled(!busy && controller->hasPreview());
        pause->setEnabled(busy);
        cancel->setEnabled(busy);
        if (busy)
            retry->setEnabled(false);
    };
    retry->setEnabled(false);
    enable();
    auto invalidate = [=] {
        if (!controller->busy()) {
            controller->invalidatePreview();
            start->setEnabled(false);
            retry->setEnabled(false);
        }
    };
    connect(local, &QLineEdit::textChanged, this, [=](const QString &) { invalidate(); });
    connect(remote, &QLineEdit::textChanged, this, [=](const QString &) { invalidate(); });
    connect(mode, &QComboBox::currentIndexChanged, this, [=](int) { invalidate(); });
    connect(settings, &SettingsPage::preferencesChanged, this, invalidate);
    connect(browse, &QPushButton::clicked, this, [=] {
        auto p = QFileDialog::getExistingDirectory(this, tr("选择备份目录"), local->text());
        if (!p.isEmpty())
            local->setText(p);
    });
    connect(scan, &QPushButton::clicked, this, [=] { controller->scan(configuration()); });
    connect(start, &QPushButton::clicked, this, [=] { controller->start(configuration()); });
    connect(pause, &QPushButton::clicked, controller, &TransferController::pauseOrResume);
    connect(cancel, &QPushButton::clicked, controller, &TransferController::cancel);
    connect(retry, &QPushButton::clicked, controller, &TransferController::retry);
    connect(controller, &TransferController::busyChanged, this, [=](bool busy) {
        enable();
        if (busy) {
            bar->setRange(0, 0);
            percent->setText(QStringLiteral("—"));
            phase->setText(tr("任务正在启动…"));
        }
    });
    connect(controller, &TransferController::previewReady, this, [=](bool) { enable(); });
    connect(controller, &TransferController::pausedChanged, this, [=](bool p) {
        pause->setText(p ? tr("继续") : tr("暂停"));
        if (p)
            phase->setText(tr("已请求暂停，将在安全边界生效"));
    });
    connect(
        controller, &TransferController::progressChanged, this,
        [=](const gui::TransferProgress &p) {
            current->setText(p.file.isEmpty() ? tr("正在准备目录任务") : p.file);
            phase->setText(p.phase);
            if (p.totalBytes) {
                bar->setRange(0, 100);
                bar->setValue(
                    static_cast<int>(qMin<quint64>(100, p.acknowledgedBytes * 100 / p.totalBytes)));
                percent->setText(QString::number(bar->value()) + "%");
            } else {
                bar->setRange(0, 0);
                percent->setText(QStringLiteral("—"));
            }
            QString eta = p.bytesPerSecond > 0 && p.totalBytes >= p.acknowledgedBytes
                              ? tr("%1 秒").arg(static_cast<quint64>(
                                    (p.totalBytes - p.acknowledgedBytes) / p.bytesPerSecond))
                              : QStringLiteral("—");
            metrics->setText(
                tr("持久确认：%1 / %2　速度：%3/秒　预计剩余：%4　文件：%5 / %6")
                    .arg(gui::formatBytes(p.acknowledgedBytes), gui::formatBytes(p.totalBytes),
                         gui::formatBytes(static_cast<quint64>(qMax(0.0, p.bytesPerSecond))), eta)
                    .arg(p.completedFiles)
                    .arg(p.totalFiles));
        });
    connect(controller, &TransferController::finished, this,
            [=](const QString &summary, bool success) {
                phase->setText(!success && controller->hasPreview()
                    ? summary + tr("；可确认备份已核验文件，错误文件不会执行。") : summary);
                bar->setRange(0, 100);
                if (success) {
                    bar->setValue(100);
                    percent->setText("100%");
                }
                retry->setEnabled(!success);
                pause->setText(tr("暂停"));
                enable();
            });
    auto summary = [=] {
        int n = 0, m = 0, u = 0, e = 0;
        for (const auto &r : controller->previewModel()->records()) {
            if (r.change == "新增")
                ++n;
            else if (r.change == "已修改")
                ++m;
            else if (r.change == "未变化")
                ++u;
            if (r.status == "错误" || !r.modeAllowed)
                ++e;
        }
        preview->setSubtitle(tr("%1 项　新增 %2　修改 %3　未变 %4　错误 %5")
                                 .arg(controller->previewModel()->rowCount())
                                 .arg(n)
                                 .arg(m)
                                 .arg(u)
                                 .arg(e) + "\n" + controller->recommendation());
    };
    connect(controller->previewModel(), &QAbstractItemModel::dataChanged, this, [=] { summary(); });
    connect(controller->previewModel(), &QAbstractItemModel::rowsInserted, this,
            [=] { summary(); });
    connect(controller->previewModel(), &QAbstractItemModel::modelReset, this, summary);
    connect(controller, &TransferController::recommendationChanged, this, [=](const QString &) { summary(); });
}
