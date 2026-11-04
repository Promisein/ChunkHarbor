#include "DashboardPage.h"
#include "QtTcpTransport.h"
#include "TransferController.h"
#include "WorkspaceStore.h"
#include "widgets/BrowserUi.h"
#include <QDateTime>
#include <QGridLayout>
#include <QHeaderView>
#include <QPainter>
#include <QSet>
#include <QTableWidget>
#include <algorithm>

namespace {
void drawIcon(QLabel *label, int kind) {
    const qreal scale = label->devicePixelRatioF();
    QPixmap image(QSize(32, 32) * scale);
    image.setDevicePixelRatio(scale);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor("#e9f2ff"));
    painter.drawRoundedRect(QRectF(0, 0, 32, 32), 8, 8);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor("#2979dc"), 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    switch (kind) {
    case 0:
        painter.drawRoundedRect(QRectF(9, 12, 14, 10), 3, 3);
        painter.drawLine(13, 8, 13, 12);
        painter.drawLine(19, 8, 19, 12);
        painter.drawLine(16, 22, 16, 25);
        break;
    case 1:
        painter.drawRoundedRect(QRectF(10, 7, 12, 18), 2, 2);
        painter.drawLine(13, 13, 19, 13);
        painter.drawLine(13, 17, 19, 17);
        break;
    case 2:
        painter.drawPolygon(QPolygonF{{8, 13}, {16, 9}, {24, 13}, {16, 17}});
        painter.drawPolyline(QPolygonF{{8, 17}, {16, 21}, {24, 17}});
        painter.drawPolyline(QPolygonF{{8, 21}, {16, 25}, {24, 21}});
        break;
    case 3:
        painter.drawEllipse(QRectF(9, 8, 14, 6));
        painter.drawLine(9, 11, 9, 21);
        painter.drawLine(23, 11, 23, 21);
        painter.drawArc(QRectF(9, 14, 14, 6), 0, -180 * 16);
        painter.drawArc(QRectF(9, 18, 14, 6), 0, -180 * 16);
        break;
    }
    painter.end();
    label->setPixmap(image);
}

QTableWidget *recentTable(InfoCard *card, const char *name) {
    auto *table = new QTableWidget(0, 2, card);
    table->setObjectName(name);
    table->setProperty("visualRole", "dashboardRecent");
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    table->setTextElideMode(Qt::ElideRight);
    table->setWordWrap(false);
    table->setShowGrid(false);
    table->verticalHeader()->hide();
    table->verticalHeader()->setDefaultSectionSize(ui::BrowserRowHeight);
    table->horizontalHeader()->hide();
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    card->body()->addWidget(table, 1);
    return table;
}

void recentRow(QTableWidget *table, int row, const QString &rawTime, const QString &description) {
    const auto parsed = QDateTime::fromString(rawTime, Qt::ISODateWithMs);
    const auto displayed = parsed.isValid() && parsed.timeSpec() != Qt::LocalTime
        ? parsed.toLocalTime().toString("yyyy-MM-dd HH:mm") : QObject::tr("时间或时区未知");
    auto *time = new QTableWidgetItem(displayed);
    time->setToolTip(ui::plainTooltip(rawTime));
    table->setItem(row, 0, time);
    auto *detail = new QTableWidgetItem(description);
    detail->setToolTip(ui::plainTooltip(description));
    table->setItem(row, 1, detail);
}
}

DashboardPage::DashboardPage(TransferController *controller, WorkspaceStore *store,
                             QtTcpTransport *transport, QWidget *parent)
    : WorkspacePage(tr("ChunkHarbor"), tr("增量备份工作区总览 · 本设备实际备份记录"), parent) {
    setObjectName("dashboardPage");
    layout_->setContentsMargins(26, 20, 26, 22);
    layout_->setSpacing(16);
    auto *cards = new QGridLayout;
    cards->setHorizontalSpacing(14);
    const QStringList names = {tr("连接状态"), tr("工作区概况"), tr("最近任务"), tr("版本概况")};
    QList<ui::ElidedLabel *> values;
    QLabel *connectionHint = nullptr;
    for (int i = 0; i < names.size(); ++i) {
        auto *card = new InfoCard(names[i], this);
        card->setProperty("visualRole", "dashboardMetric");
        card->body()->setContentsMargins(16, 14, 16, 14);
        card->body()->setSpacing(8);
        auto *titleItem = card->body()->takeAt(0);
        auto *title = titleItem->widget();
        delete titleItem;
        auto *heading = new QHBoxLayout;
        auto *icon = new QLabel(card);
        drawIcon(icon, i);
        heading->addWidget(icon);
        heading->addWidget(title, 1);
        card->body()->addLayout(heading);
        auto *value = new ui::ElidedLabel(card);
        value->setProperty("role", "metric");
        value->setObjectName(QString("dashboardMetric%1").arg(i));
        value->setMinimumHeight(value->fontMetrics().height());
        card->body()->addWidget(value);
        if (i == 0) {
            connectionHint = new QLabel(card);
            connectionHint->setObjectName("dashboardConnectionHint");
            connectionHint->setProperty("role", "muted");
            connectionHint->setWordWrap(true);
            card->body()->addWidget(connectionHint);
        }
        card->body()->addStretch();
        cards->addWidget(card, 0, i);
        cards->setColumnStretch(i, 1);
        values << value;
    }
    layout_->addLayout(cards);

    auto *lower = new QHBoxLayout;
    lower->setSpacing(16);
    auto *tasks = new InfoCard(tr("最近备份"), this);
    auto *activity = new InfoCard(tr("最近活动"), this);
    for (auto *card : {tasks, activity}) {
        card->setProperty("visualRole", "dashboardPanel");
        card->body()->setContentsMargins(16, 16, 16, 16);
        card->body()->setSpacing(10);
        lower->addWidget(card, 1);
    }
    auto *taskTable = recentTable(tasks, "dashboardRecentBackups");
    auto *logTable = recentTable(activity, "dashboardRecentActivity");
    auto *taskEmpty = ui::browserEmptyState(taskTable, tr("暂无成功备份记录"));
    auto *logEmpty = ui::browserEmptyState(logTable, tr("暂无活动记录"));
    layout_->addLayout(lower, 1);

    auto refreshBackups = [=] {
        QSet<QString> files;
        for (const auto &r : store->records())
            files.insert(r.endpoint + "/" + r.logicalPath);
        values[1]->setValue(tr("%1 个已备份文件").arg(files.size()));
        values[3]->setValue(tr("%1 条版本记录").arg(store->records().size()));
        auto records = store->records();
        std::sort(records.begin(), records.end(),
                  [](const auto &a, const auto &b) { return a.time > b.time; });
        taskTable->setRowCount(qMin(5, static_cast<int>(records.size())));
        for (int i = 0; i < taskTable->rowCount(); ++i)
            recentRow(taskTable, i, records[i].time, records[i].logicalPath);
        taskEmpty->setVisible(taskTable->rowCount() == 0);
    };
    auto refreshActivity = [=] {
        const auto &logs = store->logs();
        logTable->setRowCount(qMin(5, static_cast<int>(logs.size())));
        for (int i = 0; i < logTable->rowCount(); ++i)
            recentRow(logTable, i, logs[i].time, logs[i].message);
        logEmpty->setVisible(logTable->rowCount() == 0);
    };
    auto updateHealth = [=] {
        using Health = QtTcpTransport::HealthState;
        values[0]->setValue(transport->healthStateText());
        switch (transport->healthState()) {
        case Health::Unchecked: connectionHint->setText(tr("点击右上角「检测连接」")); break;
        case Health::Checking: connectionHint->setText(tr("正在验证服务器可用性")); break;
        case Health::Reachable: connectionHint->setText(tr("最近一次 HELLO 探测成功")); break;
        case Health::Unreachable: connectionHint->setText(tr("悬停查看失败原因")); break;
        }
    };
    connect(store, &WorkspaceStore::changed, this, refreshBackups);
    connect(store, &WorkspaceStore::logsChanged, this, refreshActivity);
    connect(controller, &TransferController::busyChanged, this,
            [=](bool busy) { values[2]->setValue(busy ? tr("任务执行中") : tr("当前空闲")); });
    connect(controller, &TransferController::progressChanged, this,
            [=](const gui::TransferProgress &p) { values[2]->setValue(p.phase); });
    connect(controller, &TransferController::finished, this,
            [=](const QString &s, bool) { values[2]->setValue(s); });
    connect(transport, &QtTcpTransport::statusChanged, this, [=](const QString &s) {
        values[0]->setToolTip(ui::plainTooltip(s));
        connectionHint->setToolTip(ui::plainTooltip(s));
    });
    connect(transport, &QtTcpTransport::healthChanged, this, updateHealth);
    values[2]->setValue(controller->busy() ? tr("任务执行中") : tr("当前空闲"));
    updateHealth();
    refreshBackups();
    refreshActivity();
}
