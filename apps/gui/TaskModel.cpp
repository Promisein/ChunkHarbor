#include "TaskModel.h"
#include <QColor>
namespace gui {
QString formatBytes(quint64 n) {
    if (n < 1024)
        return QString::number(n) + " B";
    if (n < 1024 * 1024)
        return QString::number(double(n) / 1024, 'f', 1) + " KiB";
    if (n < 1024ULL * 1024 * 1024)
        return QString::number(double(n) / (1024 * 1024), 'f', 1) + " MiB";
    return QString::number(double(n) / (1024ULL * 1024 * 1024), 'f', 1) + " GiB";
}
} // namespace gui
TaskModel::TaskModel(QObject *parent) : QAbstractTableModel(parent) {
    setObjectName("backupPreviewModel");
}
int TaskModel::rowCount(const QModelIndex &p) const { return p.isValid() ? 0 : records_.size(); }
int TaskModel::columnCount(const QModelIndex &p) const { return p.isValid() ? 0 : 5; }
QVariant TaskModel::data(const QModelIndex &i, int role) const {
    if (!i.isValid() || i.row() >= records_.size() || i.column() >= 5)
        return {};
    const auto &f = records_[i.row()];
    if (role == Qt::ToolTipRole)
        return f.localPath + "\n" + f.note;
    if (role == Qt::ForegroundRole && (i.column() == 0 || i.column() == 3)) {
        return QColor(f.status == "失败" || f.change == "错误" ? "#c45656"
                      : f.change == "新增"                     ? "#278258"
                      : f.change == "已修改"                   ? "#2979dc"
                                                               : "#748399");
    }
    if (role != Qt::DisplayRole)
        return {};
    return QStringList{f.status, f.logicalPath, gui::formatBytes(f.size), f.change,
                       f.note}[i.column()];
}
QVariant TaskModel::headerData(int i, Qt::Orientation o, int role) const {
    if (role != Qt::DisplayRole)
        return {};
    if (o == Qt::Vertical)
        return i + 1;
    return QStringList{"状态", "文件名", "大小", "变更类型", "说明"}.value(i);
}
void TaskModel::clear() {
    beginResetModel();
    records_.clear();
    endResetModel();
}
void TaskModel::upsert(const gui::FileRecord &f) {
    for (int i = 0; i < records_.size(); ++i)
        if (records_[i].logicalPath == f.logicalPath) {
            records_[i] = f;
            emit dataChanged(index(i, 0), index(i, 4));
            return;
        }
    if (records_.size() >= 10000)
        return;
    int r = records_.size();
    beginInsertRows({}, r, r);
    records_.append(f);
    endInsertRows();
}
