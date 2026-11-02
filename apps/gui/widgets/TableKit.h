#pragma once
#include <QStandardItemModel>
#include <QStringList>
#include <QTableView>
namespace ui {
QTableView *table(QWidget *parent, QStandardItemModel *model, const QStringList &headers);
void row(QStandardItemModel *model, const QStringList &values, int accentColumn = -1);
} // namespace ui
