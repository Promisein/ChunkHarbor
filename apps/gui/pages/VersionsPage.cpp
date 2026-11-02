#include "VersionsPage.h"
#include "WorkspaceStore.h"
#include "widgets/InfoCard.h"
#include "widgets/TableKit.h"
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSet>
#include <QVBoxLayout>
#include <algorithm>
VersionsPage::VersionsPage(WorkspaceStore *store, QWidget *parent)
    : WorkspacePage(tr("版本历史"), tr("本设备实际成功提交的版本记录，不代表服务器全部历史。"),
                    parent) {
    auto *row = new QHBoxLayout;
    auto *files = new InfoCard(tr("本机备份文件"), this);
    auto *list = new QListWidget(files);
    list->setObjectName("versionFiles");
    files->body()->addWidget(list);
    row->addWidget(files, 1);
    auto *history = new InfoCard(tr("历史版本"), this);
    auto *model = new QStandardItemModel(this);
    model->setObjectName("versionHistoryModel");
    auto *table =
        ui::table(history, model,
                  {tr("版本标识"), tr("提交时间"), tr("变化类型"), tr("大小"), tr("变化块数")});
    history->body()->addWidget(table);
    row->addWidget(history, 3);
    layout_->addLayout(row, 1);
    auto *details = new InfoCard(tr("版本详情"), this);
    auto *sha = new QLabel(tr("选择版本查看 SHA-256"), details);
    sha->setObjectName("versionSha");
    sha->setWordWrap(true);
    sha->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *compare = new QLabel(details);
    compare->setObjectName("versionComparison");
    compare->setWordWrap(true);
    auto *restore = new QPushButton(tr("恢复文件 · 将在 M6 提供"), details);
    restore->setEnabled(false);
    details->body()->addWidget(sha);
    details->body()->addWidget(compare);
    details->body()->addWidget(restore, 0, Qt::AlignLeft);
    layout_->addWidget(details);
    auto populate = [=] {
        model->removeRows(0, model->rowCount());
        sha->setText(tr("尚无选中版本"));
        compare->clear();
        if (!list->currentItem())
            return;
        auto key = list->currentItem()->data(Qt::UserRole).toString();
        auto records = store->records();
        std::sort(records.begin(), records.end(),
                  [](const auto &a, const auto &b) { return a.time > b.time; });
        for (const auto &r : records) {
            if (r.endpoint + "/" + r.logicalPath != key)
                continue;
            ui::row(model, {r.versionId, r.time, r.change, gui::formatBytes(r.size),
                            QString::number(r.changedBlocks)});
            auto *item = model->item(model->rowCount() - 1, 0);
            item->setData(r.sha256, Qt::UserRole);
            item->setData(tr("服务器：%1\n基线版本：%2\n本轮实际变化：%3 "
                             "个块（%4）。此信息来自提交时的上传计划，并非重新比较历史版本。 ")
                              .arg(r.endpoint, r.baseVersion.isEmpty() ? tr("无") : r.baseVersion)
                              .arg(r.changedBlocks)
                              .arg(gui::formatBytes(r.missingBytes)),
                          Qt::UserRole + 1);
        }
        if (model->rowCount())
            table->selectRow(0);
    };
    auto refresh = [=] {
        QString previous =
            list->currentItem() ? list->currentItem()->data(Qt::UserRole).toString() : QString();
        list->clear();
        QSet<QString> keys;
        for (const auto &r : store->records()) {
            QString key = r.endpoint + "/" + r.logicalPath;
            if (keys.contains(key))
                continue;
            keys.insert(key);
            auto *item = new QListWidgetItem(r.logicalPath, list);
            item->setToolTip(r.endpoint);
            item->setData(Qt::UserRole, key);
            if (key == previous)
                list->setCurrentItem(item);
        }
        if (list->currentRow() < 0 && list->count())
            list->setCurrentRow(0);
        populate();
    };
    connect(list, &QListWidget::currentRowChanged, this, [=](int) { populate(); });
    connect(table->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
            [=](const QModelIndex &i) {
                if (!i.isValid())
                    return;
                auto *item = model->item(i.row(), 0);
                sha->setText(item->data(Qt::UserRole).toString());
                compare->setText(item->data(Qt::UserRole + 1).toString());
            });
    connect(store, &WorkspaceStore::changed, this, refresh);
    refresh();
}
