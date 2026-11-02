#pragma once
#include "TransferTypes.h"
#include <QAbstractTableModel>
class TaskModel final : public QAbstractTableModel {
    Q_OBJECT
  public:
    explicit TaskModel(QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &, int role = Qt::DisplayRole) const override;
    QVariant headerData(int, Qt::Orientation, int role = Qt::DisplayRole) const override;
    void clear();
    void upsert(const gui::FileRecord &);
    const QList<gui::FileRecord> &records() const { return records_; }

  private:
    QList<gui::FileRecord> records_;
};
