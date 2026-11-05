#include "BackupPage.h"
#include "SettingsPage.h"
#include "TransferController.h"
#include "BackupSelection.h"
#include "widgets/ComboBox.h"
#include "widgets/InfoCard.h"
#include <QComboBox>
#include <QCheckBox>
#include <QDir>
#include <QFileInfo>
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
    auto folder = std::make_shared<QString>(local->text());
    local->setObjectName("backupDirectory");
    local->setPlaceholderText(tr("选择需要备份的本地目录"));
    auto *remote = new QLineEdit(prefs.value("task/prefix", "workspace").toString(), config);
    remote->setObjectName("backupPrefix");
    remote->setToolTip(tr("服务端根目录内的相对逻辑路径，例如 workspace/documents。"));
    auto *browse = new QPushButton(tr("浏览…"), config);
    browse->setObjectName("backupBrowse");
    auto *scope = new ui::ComboBox(config);
    scope->setObjectName("backupScope");
    scope->addItems({tr("整个文件夹"), tr("单个文件"), tr("多个指定文件"), tr("按规则筛选")});
    scope->setCurrentIndex(qBound(0, prefs.value("task/scope", 0).toInt(), 3));
    auto files = std::make_shared<QStringList>(prefs.value("task/selectedFiles").toStringList());
    auto *mapping = new QLineEdit(prefs.value("task/mappingRoot").toString(), config);
    mapping->setObjectName("backupMappingRoot");
    mapping->setPlaceholderText(tr("留空沿用扫描目录；选文件时请固定项目根"));
    auto *mappingBrowse = new QPushButton(tr("选择映射根…"), config);
    auto *clearFiles = new QPushButton(tr("清空文件"), config);
    auto *hidden = new QCheckBox(tr("包含隐藏属性"), config);
    hidden->setObjectName("backupHidden");
    hidden->setChecked(prefs.value("task/includeHidden", true).toBool());
    hidden->setToolTip(tr("按 Windows HIDDEN 属性判断；点开头的文件仍可参与。符号链接与重解析点不备份。"));
    auto *rules = new QWidget(config);
    auto *ruleLayout = new QHBoxLayout(rules);
    ruleLayout->setContentsMargins(0, 0, 0, 0);
    auto *include = new QLineEdit(prefs.value("task/includes").toString(), rules);
    auto *exclude = new QLineEdit(prefs.value("task/excludes").toString(), rules);
    include->setObjectName("backupIncludes");
    exclude->setObjectName("backupExcludes");
    include->setPlaceholderText(tr("包含：*.cpp;*.h;README.md（空=全部）"));
    exclude->setPlaceholderText(tr("任务排除：build/;*.tmp（优先）"));
    include->setToolTip(tr("分号分隔；精确名称、*.cpp、?、相对映射根路径；不支持 **、否定或 gitignore 语法。Windows 忽略大小写，不进行 Unicode 归一化。"));
    exclude->setToolTip(include->toolTip() + tr("\n目录规则以 / 结尾；不带 / 的任务名称也能匹配文件。"));
    ruleLayout->addWidget(new QLabel(tr("包含"), rules));
    ruleLayout->addWidget(include);
    ruleLayout->addWidget(new QLabel(tr("排除"), rules));
    ruleLayout->addWidget(exclude);
    auto *mappingHint = new QLabel(config);
    mappingHint->setObjectName("backupMappingHint");
    mappingHint->setWordWrap(true);
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
    grid->addWidget(new QLabel(tr("备份范围")), 0, 0);
    grid->addWidget(scope, 0, 1);
    grid->addWidget(hidden, 0, 2);
    grid->addWidget(mode, 0, 3);
    grid->addWidget(new QLabel(tr("本地范围")), 1, 0);
    grid->addWidget(local, 1, 1);
    grid->addWidget(browse, 1, 2, 1, 2);
    grid->addWidget(new QLabel(tr("映射根")), 2, 0);
    grid->addWidget(mapping, 2, 1);
    grid->addWidget(mappingBrowse, 2, 2);
    grid->addWidget(clearFiles, 2, 3);
    grid->addWidget(new QLabel(tr("远端路径")), 3, 0);
    grid->addWidget(remote, 3, 1);
    grid->addWidget(scan, 3, 2);
    grid->addWidget(start, 3, 3);
    grid->addWidget(rules, 4, 0, 1, 4);
    grid->setColumnStretch(1, 1);
    config->body()->addLayout(grid);
    config->body()->addWidget(mappingHint);
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
    table->setMinimumHeight(100);
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
        c.scope = static_cast<gui::BackupScope>(scope->currentIndex());
        c.mappingRoot = mapping->text().trimmed();
        c.selectedFiles = *files;
        const bool selected = scope->currentIndex() == 1 || scope->currentIndex() == 2;
        c.directory = selected ? c.mappingRoot : local->text().trimmed();
        c.inclusions = include->text().split(';', Qt::SkipEmptyParts);
        c.taskExclusions = exclude->text().split(';', Qt::SkipEmptyParts);
        c.includeHidden = hidden->isChecked();
        c.prefix = remote->text().trimmed();
        c.mode = mode->currentData().toString();
        QSettings p;
        if (!selected) p.setValue("task/directory", c.directory);
        p.setValue("task/scope", scope->currentIndex());
        p.setValue("task/mappingRoot", c.mappingRoot);
        p.setValue("task/selectedFiles", *files);
        p.setValue("task/includes", include->text());
        p.setValue("task/excludes", exclude->text());
        p.setValue("task/includeHidden", c.includeHidden);
        p.setValue("task/prefix", c.prefix);
        p.setValue("task/mode", c.mode);
        return c;
    };
    auto enable = [=] {
        bool busy = controller->busy();
        for (auto *w : QList<QWidget *>{local, remote, browse, mode, scan, scope, mapping,
                                      mappingBrowse, clearFiles, hidden, include, exclude})
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
    connect(local, &QLineEdit::textChanged, this, [=](const QString &text) {
        if (scope->currentIndex() == 0 || scope->currentIndex() == 3) *folder = text;
        invalidate();
    });
    connect(remote, &QLineEdit::textChanged, this, [=](const QString &) { invalidate(); });
    connect(mode, &QComboBox::currentIndexChanged, this, [=](int) { invalidate(); });
    connect(settings, &SettingsPage::preferencesChanged, this, invalidate);
    auto refreshScope = [=] {
        const bool selected = scope->currentIndex() == 1 || scope->currentIndex() == 2;
        local->setReadOnly(selected);
        rules->setVisible(scope->currentIndex() == 3);
        clearFiles->setVisible(selected);
        browse->setText(selected ? (scope->currentIndex() == 1 ? tr("选择文件…") : tr("添加文件…")) : tr("浏览…"));
        if (selected) {
            local->setText(files->join("; "));
            local->setToolTip(files->join('\n'));
        } else {
            local->setToolTip(tr("递归扫描范围；可与映射根不同，必须位于映射根内。"));
        }
        mappingHint->setText(tr("远端 = 路径前缀 + 相对映射根路径。%1 全局排除始终生效。")
            .arg(mapping->text().isEmpty() ? tr("映射根留空时跟随扫描目录；更改范围可能改变远端身份。")
                                         : tr("已固定映射根；改变根会创建不同逻辑身份，请核对预览。")));
    };
    connect(scope, &QComboBox::currentIndexChanged, this, [=](int) {
        if (scope->currentIndex() == 0 || scope->currentIndex() == 3)
            local->setText(*folder);
        refreshScope(); invalidate();
    });
    connect(mapping, &QLineEdit::textChanged, this, [=] { refreshScope(); invalidate(); });
    connect(include, &QLineEdit::textChanged, this, invalidate);
    connect(exclude, &QLineEdit::textChanged, this, invalidate);
    connect(hidden, &QCheckBox::toggled, this, invalidate);
    connect(clearFiles, &QPushButton::clicked, this, [=] { files->clear(); refreshScope(); invalidate(); });
    connect(mappingBrowse, &QPushButton::clicked, this, [=] {
        const auto path = QFileDialog::getExistingDirectory(this, tr("选择稳定的本地项目映射根"), mapping->text());
        if (!path.isEmpty()) mapping->setText(path);
    });
    refreshScope();
    connect(browse, &QPushButton::clicked, this, [=] {
        if (scope->currentIndex() == 1 || scope->currentIndex() == 2) {
            QStringList chosen;
            if (scope->currentIndex() == 1) {
                const auto file = QFileDialog::getOpenFileName(this, tr("选择需要备份的单个文件"), mapping->text());
                if (!file.isEmpty()) chosen.append(file);
            } else {
                chosen = QFileDialog::getOpenFileNames(this, tr("选择需要备份的文件（可重复添加跨目录文件）"), mapping->text());
            }
            if (chosen.isEmpty()) return;
            if (scope->currentIndex() == 1) *files = {chosen.front()};
            else for (const auto &file : chosen) if (!files->contains(file)) files->append(file);
            // Suggest once only. Adding an outside file must never silently change identity.
            if (mapping->text().isEmpty()) mapping->setText(gui::commonMappingRoot(*files));
            refreshScope(); invalidate();
            return;
        }
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
        preview->setSubtitle(controller->selectionSummary() + "\n" + tr("%1 项　新增 %2　修改 %3　未变 %4　错误 %5")
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
