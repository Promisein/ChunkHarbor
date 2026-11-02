#include "LogsPage.h"
#include "WorkspaceStore.h"
#include "widgets/ComboBox.h"
#include "widgets/InfoCard.h"
#include "widgets/TableKit.h"
#include <QComboBox>
#include <QDateTime>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
LogsPage::LogsPage(WorkspaceStore *store, QWidget *parent)
    : WorkspacePage(tr("日志"), tr("连接、预览和备份产生的真实本机活动记录。"), parent) {
    auto *card = new InfoCard(tr("活动日志"), this);
    auto *filters = new QHBoxLayout;
    auto *level = new ui::ComboBox(card);
    level->setObjectName("logLevel");
    level->addItems({"全部级别", "信息", "警告", "错误"});
    level->setAccessibleName(tr("日志级别"));
    auto *range = new ui::ComboBox(card);
    range->setObjectName("logRange");
    range->addItems({"全部时间", "今天", "最近一小时"});
    range->setAccessibleName(tr("时间范围"));
    auto *search = new QLineEdit(card);
    search->setObjectName("logSearch");
    search->setPlaceholderText(tr("搜索活动日志…"));
    filters->addWidget(level);
    filters->addWidget(range);
    filters->addWidget(search, 1);
    card->body()->addLayout(filters);
    auto *model = new QStandardItemModel(this);
    model->setObjectName("logModel");
    auto *table = ui::table(card, model, {"级别", "时间", "消息"});
    table->setMinimumHeight(240);
    card->body()->addWidget(table, 1);
    auto *bottom = new QHBoxLayout;
    auto *count = new QLabel(card);
    count->setProperty("role", "muted");
    auto *exportButton = new QPushButton(tr("导出日志…"), card);
    bottom->addWidget(count);
    bottom->addStretch();
    bottom->addWidget(exportButton);
    card->body()->addLayout(bottom);
    layout_->addWidget(card, 1);
    auto filter = [=] {
        model->removeRows(0, model->rowCount());
        auto now = QDateTime::currentDateTime();
        for (const auto &l : store->logs()) {
            if (level->currentIndex() > 0 && l.level != level->currentText())
                continue;
            if (range->currentIndex() == 1 &&
                QDateTime::fromString(l.time, Qt::ISODate).toLocalTime().date() != now.date())
                continue;
            if (range->currentIndex() == 2 &&
                QDateTime::fromString(l.time, Qt::ISODate) < now.addSecs(-3600))
                continue;
            if (!l.message.contains(search->text(), Qt::CaseInsensitive))
                continue;
            ui::row(model, {l.level, l.time, l.message}, 0);
        }
        count->setText(tr("共 %1 条活动日志").arg(model->rowCount()));
    };
    connect(level, &QComboBox::currentIndexChanged, this, [=](int) { filter(); });
    connect(range, &QComboBox::currentIndexChanged, this, [=](int) { filter(); });
    connect(search, &QLineEdit::textChanged, this, [=](const QString &) { filter(); });
    connect(store, &WorkspaceStore::logsChanged, this, filter);
    filter();
    connect(exportButton, &QPushButton::clicked, this, [=] {
        auto path = QFileDialog::getSaveFileName(this, tr("导出当前可见日志"), "chunkharbor.log",
                                                 tr("日志文件 (*.log);;文本文件 (*.txt)"));
        if (path.isEmpty())
            return;
        QByteArray bytes = "# ChunkHarbor 本机活动日志\n";
        for (int r = 0; r < model->rowCount(); ++r) {
            for (int c = 0; c < model->columnCount(); ++c) {
                if (c)
                    bytes += '\t';
                bytes += model->item(r, c)->text().toUtf8();
            }
            bytes += '\n';
        }
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
            QMessageBox::warning(this, tr("导出失败"), tr("无法将日志保存到所选位置。"));
    });
}
