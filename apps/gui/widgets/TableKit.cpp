#include "TableKit.h"
#include <QColor>
#include <QHeaderView>
namespace ui {
QTableView *table(QWidget *parent, QStandardItemModel *model, const QStringList &headers) {
    model->setHorizontalHeaderLabels(headers);
    auto *t = new QTableView(parent);
    t->setModel(model);
    t->setSelectionBehavior(QAbstractItemView::SelectRows);
    t->setSelectionMode(QAbstractItemView::SingleSelection);
    t->setEditTriggers(QAbstractItemView::NoEditTriggers);
    t->setShowGrid(false);
    t->setAlternatingRowColors(true);
    t->verticalHeader()->hide();
    t->verticalHeader()->setDefaultSectionSize(36);
    t->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    t->horizontalHeader()->setStretchLastSection(true);
    t->setFrameShape(QFrame::NoFrame);
    return t;
}
void row(QStandardItemModel *model, const QStringList &values, int accentColumn) {
    QList<QStandardItem *> items;
    for (int i = 0; i < values.size(); ++i) {
        auto *cell = new QStandardItem(values[i]);
        if (i == accentColumn) {
            const auto &v = values[i];
            cell->setForeground(QColor(v == "新增" || v == "信息" ? "#278258"
                                       : v == "已修改"            ? "#2979dc"
                                       : v == "警告"              ? "#ad7b25"
                                       : v == "错误"              ? "#c45656"
                                                                  : "#89929f"));
        }
        items.push_back(cell);
    }
    model->appendRow(items);
}
} // namespace ui
