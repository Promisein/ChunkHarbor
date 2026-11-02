#include "FilesPage.h"
#include "WorkspaceStore.h"
#include "widgets/ComboBox.h"
#include "widgets/InfoCard.h"
#include "widgets/TableKit.h"
#include <QComboBox>
#include <QDateTime>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QSet>
#include <QVBoxLayout>
#include <algorithm>
FilesPage::FilesPage(WorkspaceStore *store, QWidget *parent)
    : WorkspacePage(tr("文件"), tr("本机成功备份记录，不代表服务器完整目录。"), parent) {
    auto *card = new InfoCard(tr("已备份文件"), this);
    auto *filters = new QHBoxLayout;
    auto *search = new QLineEdit(card);
    search->setObjectName("fileSearch");
    search->setPlaceholderText(tr("搜索文件名、逻辑路径或本地路径…"));
    auto *status = new ui::ComboBox(card);
    status->addItems({tr("全部记录"), tr("本地文件仍存在"), tr("本地文件已移除")});
    filters->addWidget(search, 1);
    filters->addWidget(status);
    card->body()->addLayout(filters);
    auto *model = new QStandardItemModel(this);
    model->setObjectName("filesModel");
    auto *table = ui::table(card, model,
                            {tr("文件名 / 逻辑路径"), tr("本地路径"), tr("最新版本"), tr("大小"),
                             tr("最近备份时间"), tr("状态")});
    card->body()->addWidget(table, 1);
    auto *empty = new QLabel(card);
    empty->setProperty("role", "muted");
    card->body()->addWidget(empty);
    layout_->addWidget(card, 1);
    auto *details = new InfoCard(tr("文件详情"), this);
    auto *detail = new QLabel(tr("选择文件查看摘要、端点和本地路径。"), details);
    detail->setObjectName("fileDetails");
    detail->setWordWrap(true);
    detail->setTextInteractionFlags(Qt::TextSelectableByMouse);
    details->body()->addWidget(detail);
    layout_->addWidget(details);
    auto refresh = [=] {
        model->removeRows(0, model->rowCount());
        auto records = store->records();
        std::sort(records.begin(), records.end(),
                  [](const auto &a, const auto &b) { return a.time > b.time; });
        QSet<QString> seen;
        for (const auto &r : records) {
            auto key = r.endpoint + "/" + r.logicalPath;
            if (seen.contains(key))
                continue;
            seen.insert(key);
            bool exists = QFileInfo::exists(r.localPath);
            if (status->currentIndex() == 1 && !exists)
                continue;
            if (status->currentIndex() == 2 && exists)
                continue;
            if (!(r.logicalPath + " " + r.localPath).contains(search->text(), Qt::CaseInsensitive))
                continue;
            ui::row(model, {r.logicalPath, r.localPath, r.versionId, gui::formatBytes(r.size),
                            r.time, exists ? tr("已备份") : tr("本地已移除")});
            model->item(model->rowCount() - 1, 0)
                ->setData(tr("服务器：%1\n逻辑路径：%2\n本地路径：%3\nSHA-256：%4\n记录时间：%"
                             "5\n此处显示本机提交时的记录，未重新检查服务器。 ")
                              .arg(r.endpoint, r.logicalPath, r.localPath, r.sha256, r.time),
                          Qt::UserRole);
        }
        empty->setText(model->rowCount() ? tr("共 %1 个文件").arg(model->rowCount())
                                         : tr("暂无匹配记录。成功备份后将在这里显示。"));
        detail->setText(tr("选择文件查看详情"));
    };
    connect(search, &QLineEdit::textChanged, this, [=](const QString &) { refresh(); });
    connect(status, &QComboBox::currentIndexChanged, this, [=](int) { refresh(); });
    connect(store, &WorkspaceStore::changed, this, refresh);
    connect(table->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
            [=](const QModelIndex &i) {
                if (i.isValid())
                    detail->setText(model->item(i.row(), 0)->data(Qt::UserRole).toString());
            });
    refresh();
}
