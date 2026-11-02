#pragma once
#include "chunkharbor/protocol/FilesList.h"
#include <QAbstractTableModel>
class ServerFilesModel final : public QAbstractTableModel {
    Q_OBJECT
  public:
    explicit ServerFilesModel(QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &, int role = Qt::DisplayRole) const override;
    QVariant headerData(int, Qt::Orientation, int role = Qt::DisplayRole) const override;
    void setPage(chunkharbor::protocol::FilesResult);
    const chunkharbor::protocol::FilesResult &page() const { return page_; }
  private:
    chunkharbor::protocol::FilesResult page_;
};
