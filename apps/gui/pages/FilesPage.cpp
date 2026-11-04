#include "FilesPage.h"
#include "FileListController.h"
#include "DownloadController.h"
#include "widgets/InfoCard.h"
#include "widgets/BrowserUi.h"
#include <QApplication>
#include <QClipboard>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QResizeEvent>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QStyle>
#include <QTableView>
#include <QVBoxLayout>

namespace {
QString readableSize(quint64 bytes) {
    const char *units[] = {"B", "KB", "MB", "GB", "TB", "PB", "EB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024 && unit < 6) { value /= 1024; ++unit; }
    return unit ? QString::number(value, 'f', 1) + " " + units[unit]
                : QString::number(bytes) + " B";
}
using ui::ElidedLabel;
class FilePresentationModel final : public QSortFilterProxyModel {
  public:
    using QSortFilterProxyModel::QSortFilterProxyModel;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
        if (orientation == Qt::Horizontal && role == Qt::TextAlignmentRole)
            return int((section == 1 ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter);
        if (orientation == Qt::Horizontal && role == Qt::DisplayRole) {
            if (section == 0) return tr("文件名称 / 相对路径");
            if (section == 1) return tr("大小");
        }
        return QSortFilterProxyModel::headerData(section, orientation, role);
    }
    QVariant data(const QModelIndex &index, int role) const override {
        if (index.column() == 1) {
            if (role == Qt::DisplayRole)
                return readableSize(QSortFilterProxyModel::data(index, Qt::UserRole).toULongLong());
            if (role == Qt::TextAlignmentRole) return int(Qt::AlignRight | Qt::AlignVCenter);
        }
        return QSortFilterProxyModel::data(index, role);
    }
};
}
FilesPage::FilesPage(FileListController *controller, QWidget *parent, DownloadController *download)
    : WorkspacePage(tr("文件"), tr("管理已备份的服务器文件"), parent) {
    if (!download) download = new DownloadController(this);
    ui::browserPage(layout_);
    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setObjectName("filesSplitter");
    ui::browserSplitter(splitter);
    auto *list = new QWidget(splitter);
    list->setMinimumWidth(320);
    auto *body = new QVBoxLayout(list);
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(12);
    auto *controls = new QHBoxLayout;
    auto *search = new QLineEdit(list);
    search->setObjectName("fileSearch");
    search->setPlaceholderText(tr("筛选当前页的文件路径…"));
    search->setToolTip(tr("仅筛选当前页，不搜索服务器其他页面。"));
    controls->addWidget(search, 1);
    auto *refresh = new QPushButton(list);
    refresh->setObjectName("filesRefresh");
    refresh->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
    refresh->setToolTip(tr("刷新列表"));
    refresh->setAccessibleName(tr("刷新列表"));
    controls->addWidget(refresh);
    auto *cancel = new QPushButton(tr("取消查询"), list);
    cancel->setObjectName("filesCancel");
    controls->addWidget(cancel);
    auto *toggle = new QPushButton(tr("详情"), list);
    toggle->setObjectName("filesToggleDetails");
    toggle->setCheckable(true);
    toggle->setChecked(true);
    toggle->setToolTip(tr("显示或收起文件详情，拖动分隔条调整宽度。"));
    controls->addWidget(toggle);
    body->addLayout(controls);
    auto *proxy = new FilePresentationModel(this);
    proxy->setSourceModel(controller->model());
    proxy->setFilterKeyColumn(0);
    proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    auto *table = new QTableView(list);
    table->setObjectName("filesTable");
    table->setModel(proxy);
    ui::browserList(table);
    auto *empty = ui::browserEmptyState(table, tr("当前页没有文件"));
    table->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setAlternatingRowColors(true);
    table->setShowGrid(false);
    table->setWordWrap(false);
    table->setTextElideMode(Qt::ElideMiddle);
    table->verticalHeader()->hide();
    table->verticalHeader()->setDefaultSectionSize(ui::BrowserRowHeight);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
    table->setColumnWidth(1, 100);
    table->hideColumn(2);
    table->hideColumn(3);
    body->addWidget(table, 1);
    auto *pagination = new QHBoxLayout;
    auto *count = new QLabel(list);
    count->setObjectName("filesCount");
    count->setProperty("role", "muted");
    pagination->addWidget(count, 1);
    auto *previous = new QPushButton(tr("上一页"), list);
    previous->setObjectName("filesPrevious");
    previous->setToolTip(tr("返回最近浏览的页面，最多保留 128 页游标。"));
    auto *next = new QPushButton(tr("下一页"), list);
    next->setObjectName("filesNext");
    pagination->addWidget(previous);
    pagination->addWidget(next);
    body->addLayout(pagination);
    auto *status = new QLabel(list);
    status->setObjectName("filesStatus");
    status->setTextFormat(Qt::PlainText);
    status->setWordWrap(true);
    status->setProperty("role", "muted");
    body->addWidget(status);

    auto *details = new InfoCard(tr("文件详情"), splitter);
    details->setObjectName("filesDetailsPanel");
    ui::detailPanel(details);
    auto *fileIcon = new QLabel(details);
    fileIcon->setAlignment(Qt::AlignCenter);
    fileIcon->setPixmap(style()->standardIcon(QStyle::SP_FileIcon).pixmap(40, 40));
    details->body()->addWidget(fileIcon);
    auto *name = new ElidedLabel(details);
    name->setObjectName("fileName");
    name->setProperty("role", "detailName");
    name->setAlignment(Qt::AlignCenter);
    details->body()->addWidget(name);
    auto *type = new QLabel(details);
    type->setTextFormat(Qt::PlainText);
    type->setAlignment(Qt::AlignCenter);
    type->setProperty("role", "muted");
    details->body()->addWidget(type);
    auto field = [=](const QString &title, const char *objectName, bool copy) {
        return ui::detailField(details, title, objectName, copy);
    };
    auto *path = field(tr("远程路径"), "fileDetails", true);
    auto *size = field(tr("文件大小"), "fileSize", false);
    auto *version = field(tr("当前版本"), "fileVersion", true);
    auto *sha = field("SHA-256", "fileSha", true);
    details->body()->addStretch(1);
    auto *restore = new QPushButton(tr("下载文件"), details);
    restore->setObjectName("filesRestore");
    restore->setProperty("primary", true);
    restore->setIcon(style()->standardIcon(QStyle::SP_ArrowDown));
    details->body()->addWidget(restore);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 0);
    splitter->setSizes({650, 320});
    connect(toggle, &QPushButton::toggled, details, &QWidget::setVisible);
    layout_->addWidget(splitter, 1);

    transferBar_ = new QFrame(this);
    transferBar_->setObjectName("filesTransferBar");
    auto *transfer = new QVBoxLayout(transferBar_);
    transfer->setContentsMargins(24, 10, 24, 10);
    transfer->setSpacing(6);
    auto *transferRow = new QHBoxLayout;
    auto *downloadStatus = new ElidedLabel(transferBar_);
    downloadStatus->setObjectName("filesDownloadStatus");
    transferRow->addWidget(downloadStatus, 1);
    auto *downloadVersion = new QLabel(transferBar_);
    downloadVersion->setObjectName("filesDownloadVersion");
    downloadVersion->setTextInteractionFlags(Qt::TextSelectableByMouse);
    transferRow->addWidget(downloadVersion);
    auto *downloadBytes = new QLabel(transferBar_);
    downloadBytes->setObjectName("filesDownloadBytes");
    downloadBytes->setProperty("role", "muted");
    transferRow->addWidget(downloadBytes);
    auto *downloadCancel = new QPushButton(tr("取消下载"), transferBar_);
    downloadCancel->setObjectName("filesDownloadCancel");
    transferRow->addWidget(downloadCancel);
    transfer->addLayout(transferRow);
    auto *downloadProgress = new QProgressBar(transferBar_);
    downloadProgress->setObjectName("filesDownloadProgress");
    downloadProgress->setRange(0, 100);
    downloadProgress->setTextVisible(false);
    transfer->addWidget(downloadProgress);
    layout_->addWidget(transferBar_);
    auto updateDownload = [=] {
        restore->setEnabled(!download->busy() && !controller->busy() && table->currentIndex().isValid());
        downloadCancel->setVisible(download->busy());
        downloadCancel->setEnabled(download->busy());
        const bool started = !download->fileName().isEmpty();
        downloadStatus->setValue(started ? tr("%1 · %2").arg(download->fileName(), download->status())
                                        : tr("✓ 当前没有传输任务"));
        downloadVersion->setVisible(!download->historicalVersion().isEmpty());
        downloadVersion->setText(tr("版本 %1").arg(download->historicalVersion()));
        downloadBytes->setVisible(started);
        downloadBytes->setText(tr("%1 / %2 · %3%")
            .arg(readableSize(download->received()), readableSize(download->total()))
            .arg(download->total() ? 100 * download->received() / download->total() : download->succeeded() ? 100 : 0));
        downloadBytes->setToolTip(tr("已下载 %1 / %2 字节").arg(download->received()).arg(download->total()));
        downloadProgress->setVisible(download->busy());
        downloadProgress->setValue(download->total()
            ? static_cast<int>(100 * download->received() / download->total()) : download->succeeded() ? 100 : 0);
    };
    auto updateDetails = [=] {
        const auto source = proxy->mapToSource(table->currentIndex());
        const auto &files = controller->model()->page().files;
        const bool selected = source.isValid() && static_cast<std::size_t>(source.row()) < files.size();
        if (selected) {
            const auto &file = files[static_cast<std::size_t>(source.row())];
            const auto logical = QString::fromUtf8(file.path);
            name->setValue(QFileInfo(logical).fileName());
            const auto suffix = QFileInfo(logical).suffix().toUpper();
            type->setText(suffix.isEmpty() ? tr("文件") : tr("%1 文件").arg(suffix));
            path->setValue(logical);
            size->setValue(readableSize(file.size));
            size->setToolTip(tr("%1 字节").arg(file.size));
            version->setValue(QString::fromUtf8(file.versionId));
            sha->setValue(QString::fromUtf8(file.sha256));
        } else {
            name->setValue(tr("选择文件查看详情"));
            type->clear();
            for (auto *value : {path, size, version, sha}) value->setValue({});
        }
        for (auto *button : details->findChildren<QPushButton *>())
            if (button != restore) button->setEnabled(selected);
        updateDownload();
    };
    connect(download, &DownloadController::changed, this, updateDownload);
    connect(controller, &FileListController::changed, this, updateDownload);
    connect(table->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [=](const QModelIndex &, const QModelIndex &) { updateDetails(); });
    connect(controller->model(), &QAbstractItemModel::modelReset, this, updateDetails);
    connect(downloadCancel, &QPushButton::clicked, download, &DownloadController::cancel);
    connect(restore, &QPushButton::clicked, this, [=] {
        if (download->busy() || controller->busy()) return;
        const auto source = proxy->mapToSource(table->currentIndex());
        const auto &files = controller->model()->page().files;
        if (!source.isValid() || static_cast<std::size_t>(source.row()) >= files.size()) return;
        const auto selected = files[static_cast<std::size_t>(source.row())];
        const auto host = controller->host();
        const auto port = controller->port();
        const auto target = QFileDialog::getSaveFileName(this, tr("下载文件（不覆盖已有文件）"),
            QFileInfo(QString::fromUtf8(selected.path)).fileName(), QString(), nullptr,
            QFileDialog::DontConfirmOverwrite);
        if (!target.isEmpty()) download->start(host, port, selected, target);
    });
    auto updateCount = [=] {
        empty->setText(search->text().isEmpty() ? tr("当前页没有文件") : tr("当前页无匹配文件"));
        empty->setVisible(proxy->rowCount() == 0);
        count->setText(tr("第 %1 页 · 本页 %2 个文件%3")
            .arg(controller->pageNumber()).arg(controller->model()->rowCount())
            .arg(search->text().isEmpty() ? QString{} : tr("（匹配 %1）").arg(proxy->rowCount())));
    };
    auto update = [=] {
        refresh->setEnabled(!controller->busy());
        previous->setEnabled(!controller->busy() && controller->hasPrevious());
        next->setEnabled(!controller->busy() && controller->hasNext());
        cancel->setVisible(controller->busy());
        cancel->setEnabled(controller->busy());
        status->setText(controller->status());
        updateCount();
    };
    connect(refresh, &QPushButton::clicked, controller, &FileListController::refresh);
    connect(previous, &QPushButton::clicked, controller, &FileListController::previous);
    connect(next, &QPushButton::clicked, controller, &FileListController::next);
    connect(cancel, &QPushButton::clicked, controller, &FileListController::cancel);
    connect(controller, &FileListController::changed, this, update);
    connect(controller->model(), &QAbstractItemModel::modelReset, this, updateCount);
    connect(search, &QLineEdit::textChanged, this, [=](const QString &text) {
        proxy->setFilterFixedString(text);
        // A filtered-out current index must not leave a stale download target.
        updateDetails();
        updateCount();
    });
    updateDetails();
    update();
}
