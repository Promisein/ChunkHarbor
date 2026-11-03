#include "DashboardPage.h"
#include "QtTcpTransport.h"
#include "TransferController.h"
#include "WorkspaceStore.h"
#include "widgets/InfoCard.h"
#include <QHBoxLayout>
#include <QLabel>
#include <QSet>
#include <QVBoxLayout>
#include <algorithm>
DashboardPage::DashboardPage(TransferController *controller, WorkspaceStore *store,
                             QtTcpTransport *transport, QWidget *parent)
    : WorkspacePage(tr("ChunkHarbor"), tr("增量备份工作区总览 · 本设备实际备份记录"), parent) {
    auto *cards = new QHBoxLayout;
    const QStringList names = {tr("连接状态"), tr("工作区概况"), tr("最近任务"), tr("版本概况")};
    QList<QLabel *> values;
    for (int i = 0; i < 4; ++i) {
        auto *card = new InfoCard(names[i], this);
        auto *label = new QLabel(card);
        label->setWordWrap(true);
        label->setProperty("role", "metric");
        label->setObjectName(QString("dashboardMetric%1").arg(i));
        card->body()->addWidget(label);
        cards->addWidget(card, 1);
        values << label;
    }
    layout_->addLayout(cards);
    values[0]->setText(transport->healthStateText());
    auto *lower = new QHBoxLayout;
    auto *tasks = new InfoCard(tr("最近备份"), this);
    auto *activity = new InfoCard(tr("最近活动"), this);
    auto *taskText = new QLabel(tasks);
    auto *logText = new QLabel(activity);
    taskText->setWordWrap(true);
    logText->setWordWrap(true);
    taskText->setTextInteractionFlags(Qt::TextSelectableByMouse);
    logText->setTextInteractionFlags(Qt::TextSelectableByMouse);
    tasks->body()->addWidget(taskText);
    tasks->body()->addStretch();
    activity->body()->addWidget(logText);
    activity->body()->addStretch();
    lower->addWidget(tasks, 3);
    lower->addWidget(activity, 2);
    layout_->addLayout(lower, 1);
    auto refresh = [=] {
        QSet<QString> files;
        for (const auto &r : store->records())
            files.insert(r.endpoint + "/" + r.logicalPath);
        values[1]->setText(tr("%1 个已备份文件").arg(files.size()));
        values[3]->setText(tr("%1 条版本记录").arg(store->records().size()));
        if (!controller->busy())
            values[2]->setText(tr("当前空闲"));
        QStringList recent;
        auto records = store->records();
        std::sort(records.begin(), records.end(),
                  [](const auto &a, const auto &b) { return a.time > b.time; });
        for (int i = 0; i < qMin(5, static_cast<int>(records.size())); ++i)
            recent << records[i].time + "  " + records[i].logicalPath;
        taskText->setText(recent.isEmpty() ? tr("还没有成功备份记录。请在备份页扫描并开始任务。")
                                           : recent.join("\n\n"));
        QStringList logs;
        const auto &list = store->logs();
        for (int i = 0; i < qMin(5, static_cast<int>(list.size())); ++i)
            logs << list[i].time + "  " + list[i].message;
        logText->setText(logs.isEmpty() ? tr("暂无活动记录") : logs.join("\n\n"));
    };
    connect(store, &WorkspaceStore::changed, this, refresh);
    connect(store, &WorkspaceStore::logsChanged, this, refresh);
    connect(controller, &TransferController::busyChanged, this,
            [=](bool busy) { values[2]->setText(busy ? tr("任务执行中") : tr("当前空闲")); });
    connect(controller, &TransferController::progressChanged, this,
            [=](const gui::TransferProgress &p) { values[2]->setText(p.phase); });
    connect(controller, &TransferController::finished, this,
            [=](const QString &s, bool) { values[2]->setText(s); });
    connect(transport, &QtTcpTransport::statusChanged, this, [=](const QString &s) {
        values[0]->setToolTip(s);
    });
    connect(transport, &QtTcpTransport::healthChanged, this, [=] {
        values[0]->setText(transport->healthStateText());
    });
    refresh();
}
