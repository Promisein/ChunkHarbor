#include "FilesPage.h"
#include "FileListController.h"
#include "widgets/InfoCard.h"
#include <QHeaderView>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QSortFilterProxyModel>
#include <QTableView>
#include <QVBoxLayout>
FilesPage::FilesPage(FileListController *controller, QWidget *parent)
    : WorkspacePage(tr("文件"), tr("查询服务器当前已提交文件；版本历史页保留本机备份记录。"), parent) {
    auto *card = new InfoCard(tr("服务器文件"), this);
    auto *controls = new QHBoxLayout;
    auto *refresh = new QPushButton(tr("刷新列表"), card);
    refresh->setObjectName("filesRefresh");
    refresh->setProperty("primary", true);
    auto *previous = new QPushButton(tr("上一页"), card);
    previous->setObjectName("filesPrevious");
    previous->setToolTip(tr("返回最近浏览的页面，最多保留 128 页游标。"));
    auto *next = new QPushButton(tr("下一页"), card);
    next->setObjectName("filesNext");
    auto *cancel = new QPushButton(tr("取消查询"), card);
    cancel->setObjectName("filesCancel");
    for (auto *button : {refresh, previous, next, cancel}) controls->addWidget(button);
    controls->addStretch();
    card->body()->addLayout(controls);
    auto *endpoint = new QLabel(card);
    endpoint->setObjectName("filesEndpoint");
    card->body()->addWidget(endpoint);
    auto *search = new QLineEdit(card);
    search->setObjectName("fileSearch");
    search->setPlaceholderText(tr("筛选当前页的逻辑路径…"));
    card->body()->addWidget(search);
    auto *proxy = new QSortFilterProxyModel(this);
    proxy->setSourceModel(controller->model());
    proxy->setFilterKeyColumn(0);
    proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    auto *table = new QTableView(card);
    table->setObjectName("filesTable");
    table->setModel(proxy);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setAlternatingRowColors(true);
    table->setShowGrid(false);
    table->verticalHeader()->hide();
    table->verticalHeader()->setDefaultSectionSize(36);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->setColumnWidth(1, 150);
    table->setColumnWidth(2, 170);
    table->setColumnWidth(3, 190);
    card->body()->addWidget(table, 1);
    auto *status = new QLabel(card);
    status->setObjectName("filesStatus");
    status->setTextFormat(Qt::PlainText);
    status->setWordWrap(true);
    card->body()->addWidget(status);
    auto *count = new QLabel(card);
    count->setObjectName("filesCount");
    count->setProperty("role", "muted");
    card->body()->addWidget(count);
    layout_->addWidget(card, 1);
    auto *details = new InfoCard(tr("文件详情"), this);
    auto *detail = new QPlainTextEdit(details);
    detail->setPlainText(tr("选择当前页文件查看详情。"));
    detail->setObjectName("fileDetails");
    detail->setReadOnly(true);
    detail->setMinimumHeight(150);
    detail->setMaximumHeight(220);
    details->body()->addWidget(detail);
    auto *restore = new QPushButton(tr("恢复文件（尚未接入）"), details);
    restore->setObjectName("filesRestore");
    restore->setEnabled(false);
    details->body()->addWidget(restore, 0, Qt::AlignLeft);
    layout_->addWidget(details);
    auto updateCount = [=] {
        count->setText(tr("当前页匹配 %1 / %2 条。筛选不搜索其他页面。").arg(proxy->rowCount()).arg(controller->model()->rowCount()));
    };
    auto update = [=] {
        refresh->setEnabled(!controller->busy());
        previous->setEnabled(!controller->busy() && controller->hasPrevious());
        next->setEnabled(!controller->busy() && controller->hasNext());
        cancel->setEnabled(controller->busy());
        endpoint->setText(tr("服务器：%1").arg(controller->endpoint()));
        status->setText(controller->status());
        updateCount();
    };
    connect(refresh, &QPushButton::clicked, controller, &FileListController::refresh);
    connect(previous, &QPushButton::clicked, controller, &FileListController::previous);
    connect(next, &QPushButton::clicked, controller, &FileListController::next);
    connect(cancel, &QPushButton::clicked, controller, &FileListController::cancel);
    connect(controller, &FileListController::changed, this, update);
    connect(controller->model(), &QAbstractItemModel::modelReset, this, [=] {
        detail->setPlainText(tr("选择当前页文件查看详情。"));
        updateCount();
    });
    connect(search, &QLineEdit::textChanged, this, [=](const QString &text) {
        proxy->setFilterFixedString(text);
        detail->setPlainText(tr("选择当前页文件查看详情。"));
        updateCount();
    });
    connect(table->selectionModel(), &QItemSelectionModel::currentRowChanged, this, [=](const QModelIndex &index) {
        if (index.isValid()) detail->setPlainText(tr("服务器：%1\n%2").arg(controller->endpoint(), proxy->data(index, Qt::UserRole + 1).toString()));
        else detail->setPlainText(tr("选择当前页文件查看详情。"));
    });
    update();
}
